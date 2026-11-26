#include "mysql_filter_pushdown.hpp"
#include "mysql_utils.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

namespace duckdb {

static string TransformFilterExpr(const string &column_name, const Expression &expr);

static string CreateConjunctionExpr(const string &column_name, const vector<unique_ptr<Expression>> &children,
                                    const string &op) {
	vector<string> parts;
	for (auto &child : children) {
		auto part = TransformFilterExpr(column_name, *child);
		if (part.empty()) {
			return string();
		}
		parts.push_back(std::move(part));
	}
	if (parts.empty()) {
		return string();
	}
	return "(" + StringUtil::Join(parts, " " + op + " ") + ")";
}

static bool IsDirectColumnRef(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_REF ||
	       expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF;
}

static string TransformFilterExpr(const string &column_name, const Expression &expr) {
	if (BoundComparisonExpression::IsComparison(expr)) {
		auto &cmp = expr.Cast<BoundFunctionExpression>();
		auto cmp_type = cmp.GetExpressionType();
		auto &left = BoundComparisonExpression::Left(cmp);
		auto &right = BoundComparisonExpression::Right(cmp);
		const Value *constant = nullptr;
		if (IsDirectColumnRef(left) && right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			constant = &right.Cast<BoundConstantExpression>().GetValue();
		} else if (left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT && IsDirectColumnRef(right)) {
			constant = &left.Cast<BoundConstantExpression>().GetValue();
			cmp_type = FlipComparisonExpression(cmp_type);
		} else {
			return string();
		}
		if (cmp_type != ExpressionType::COMPARE_EQUAL && cmp_type != ExpressionType::COMPARE_NOTEQUAL &&
		    cmp_type != ExpressionType::COMPARE_LESSTHAN && cmp_type != ExpressionType::COMPARE_GREATERTHAN &&
		    cmp_type != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
		    cmp_type != ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
			return string();
		}
		string constant_string;
		if (!MySQLUtils::TryTransformConstant(*constant, constant_string)) {
			return string();
		}
		auto operator_string = MySQLFilterPushdown::TransformComparison(cmp_type);
		return StringUtil::Format("%s %s %s", column_name, operator_string, constant_string);
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto op = conj.GetExpressionType() == ExpressionType::CONJUNCTION_AND ? "AND" : "OR";
		return CreateConjunctionExpr(column_name, conj.GetChildren(), op);
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		auto &children = op.GetChildren();
		if (children.empty() || !IsDirectColumnRef(*children[0])) {
			return string();
		}
		switch (op.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
			return column_name + " IS NULL";
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			return column_name + " IS NOT NULL";
		case ExpressionType::COMPARE_IN: {
			if (children.size() <= 1) {
				return string();
			}
			string in_list;
			for (idx_t i = 1; i < children.size(); i++) {
				if (children[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
					return string();
				}
				string constant_string;
				if (!MySQLUtils::TryTransformConstant(children[i]->Cast<BoundConstantExpression>().GetValue(),
				                                      constant_string)) {
					return string();
				}
				if (!in_list.empty()) {
					in_list += ", ";
				}
				in_list += constant_string;
			}
			return column_name + " IN (" + in_list + ")";
		}
		default:
			return string();
		}
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.Function().GetName() == OptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<OptionalFilterFunctionData>();
			return data.child_filter_expr ? TransformFilterExpr(column_name, *data.child_filter_expr) : string();
		}
		if (func.Function().GetName() == SelectivityOptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>();
			return data.child_filter_expr ? TransformFilterExpr(column_name, *data.child_filter_expr) : string();
		}
		if (func.Function().GetName() == DynamicFilterScalarFun::NAME) {
			return string();
		}
		return string();
	}
	default:
		return string();
	}
}

string MySQLFilterPushdown::TransformComparison(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return "=";
	case ExpressionType::COMPARE_NOTEQUAL:
		return "!=";
	case ExpressionType::COMPARE_LESSTHAN:
		return "<";
	case ExpressionType::COMPARE_GREATERTHAN:
		return ">";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return "<=";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ">=";
	default:
		throw NotImplementedException("Unsupported expression type");
	}
}

string MySQLFilterPushdown::TransformFilter(string &column_name, TableFilter &filter) {
	if (filter.filter_type == TableFilterType::EXPRESSION_FILTER) {
		auto &expr_filter = filter.Cast<ExpressionFilter>();
		if (!expr_filter.expr) {
			return string();
		}
		return TransformFilterExpr(column_name, *expr_filter.expr);
	}
	return string();
}

string MySQLFilterPushdown::TransformFilters(const vector<column_t> &column_ids, optional_ptr<TableFilterSet> filters,
                                             const vector<string> &names, vector<column_t> *unpushed_columns) {
	if (!filters || !filters->HasFilters()) {
		// no filters
		return string();
	}
	string result;
	for (auto &entry : *filters) {
		auto filter_col_idx = entry.GetIndex().GetIndexUnsafe();
		if (filter_col_idx >= column_ids.size()) {
			if (unpushed_columns) {
				unpushed_columns->push_back(filter_col_idx);
			}
			continue;
		}
		auto col_idx = column_ids[filter_col_idx];
		if (col_idx == COLUMN_IDENTIFIER_ROW_ID || col_idx >= names.size()) {
			if (unpushed_columns) {
				unpushed_columns->push_back(filter_col_idx);
			}
			continue;
		}
		auto column_name = MySQLUtils::WriteIdentifier(names[col_idx]);
		auto &filter = entry.Filter();
		auto new_filter = TransformFilter(column_name, const_cast<TableFilter &>(filter));
		if (new_filter.empty()) {
			if (unpushed_columns) {
				unpushed_columns->push_back(filter_col_idx);
			}
			continue;
		}
		if (!result.empty()) {
			result += " AND ";
		}
		result += new_filter;
	}
	return result;
}

} // namespace duckdb
