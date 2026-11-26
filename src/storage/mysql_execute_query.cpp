#include "storage/mysql_execute_query.hpp"
#include "storage/mysql_table_entry.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "storage/mysql_catalog.hpp"
#include "storage/mysql_transaction.hpp"
#include "mysql_connection.hpp"
#include "mysql_filter_pushdown.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/execution/operator/filter/physical_filter.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"

namespace duckdb {

MySQLExecuteQuery::MySQLExecuteQuery(PhysicalPlan &physical_plan, LogicalOperator &op, string op_name_p,
                                     TableCatalogEntry &table, string query_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1), op_name(std::move(op_name_p)),
      table(table), query(std::move(query_p)) {
}

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
class MySQLExecuteQueryGlobalState : public GlobalSinkState {
public:
	explicit MySQLExecuteQueryGlobalState() : affected_rows(0) {
	}

	idx_t affected_rows;
};

unique_ptr<GlobalSinkState> MySQLExecuteQuery::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<MySQLExecuteQueryGlobalState>();
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType MySQLExecuteQuery::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	return SinkResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType MySQLExecuteQuery::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                             OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<MySQLExecuteQueryGlobalState>();
	auto &transaction = MySQLTransaction::Get(context, table.catalog);
	auto &connection = transaction.GetConnection();
	auto result = connection.Query(query, MySQLResultStreaming::FORCE_MATERIALIZATION);
	gstate.affected_rows = result->AffectedRows();
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// GetData
//===--------------------------------------------------------------------===//
SourceResultType MySQLExecuteQuery::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                    OperatorSourceInput &input) const {
	auto &insert_gstate = sink_state->Cast<MySQLExecuteQueryGlobalState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(insert_gstate.affected_rows));

	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
string MySQLExecuteQuery::GetName() const {
	return op_name;
}

InsertionOrderPreservingMap<string> MySQLExecuteQuery::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table.name;
	return result;
}

//===--------------------------------------------------------------------===//
// Plan
//===--------------------------------------------------------------------===//
static vector<string> CollectOutputColumnNames(const PhysicalOperator &op) {
	if (op.type == PhysicalOperatorType::TABLE_SCAN) {
		auto &table_scan = op.Cast<PhysicalTableScan>();
		vector<string> out;
		if (!table_scan.projection_ids.empty()) {
			out.reserve(table_scan.projection_ids.size());
			for (auto proj_idx : table_scan.projection_ids) {
				if (proj_idx < table_scan.column_ids.size()) {
					auto col_id = table_scan.column_ids[proj_idx].GetPrimaryIndex();
					out.push_back(col_id == COLUMN_IDENTIFIER_ROW_ID
					                  ? "rowid"
					                  : (col_id < table_scan.names.size() ? table_scan.names[col_id] : ""));
				} else {
					out.emplace_back();
				}
			}
		} else {
			out.reserve(table_scan.column_ids.size());
			for (const auto &col_index : table_scan.column_ids) {
				auto col_id = col_index.GetPrimaryIndex();
				out.push_back(col_id == COLUMN_IDENTIFIER_ROW_ID
				                  ? "rowid"
				                  : (col_id < table_scan.names.size() ? table_scan.names[col_id] : ""));
			}
		}
		return out;
	}
	if (op.type == PhysicalOperatorType::FILTER && !op.children.empty()) {
		return CollectOutputColumnNames(op.children[0]);
	}
	if (op.type == PhysicalOperatorType::PROJECTION && !op.children.empty()) {
		auto child_cols = CollectOutputColumnNames(op.children[0]);
		auto &proj = op.Cast<PhysicalProjection>();
		vector<string> out;
		out.reserve(proj.select_list.size());
		for (const auto &expr : proj.select_list) {
			if (expr->GetExpressionClass() == ExpressionClass::BOUND_REF) {
				auto &ref = expr->Cast<BoundReferenceExpression>();
				out.push_back(ref.index < child_cols.size() ? child_cols[ref.index] : expr->alias);
			} else if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				out.push_back(expr->alias);
			} else {
				out.emplace_back();
			}
		}
		return out;
	}
	return {};
}

static string TransformFilterExpressionToMySQL(const Expression &expr, const vector<string> &column_names,
                                               const string &statement) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONSTANT: {
		auto &bound_const = expr.Cast<BoundConstantExpression>();
		return MySQLUtils::TransformConstant(bound_const.value);
	}
	case ExpressionClass::BOUND_REF: {
		auto &ref = expr.Cast<BoundReferenceExpression>();
		string col_name;
		if (ref.index < column_names.size() && !column_names[ref.index].empty()) {
			col_name = column_names[ref.index];
		} else if (!ref.alias.empty()) {
			col_name = ref.alias;
		} else {
			throw NotImplementedException("Unable to resolve column reference in MySQL %s filter", statement);
		}
		return MySQLUtils::WriteIdentifier(col_name);
	}
	case ExpressionClass::BOUND_COLUMN_REF: {
		auto &ref = expr.Cast<BoundColumnRefExpression>();
		if (ref.alias.empty()) {
			throw NotImplementedException("Unable to resolve column reference in MySQL %s filter", statement);
		}
		return MySQLUtils::WriteIdentifier(ref.alias);
	}
	case ExpressionClass::BOUND_COMPARISON: {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		string op_str;
		switch (comp.type) {
		case ExpressionType::COMPARE_EQUAL:
			op_str = "=";
			break;
		case ExpressionType::COMPARE_NOTEQUAL:
			op_str = "!=";
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			op_str = "<";
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			op_str = ">";
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			op_str = "<=";
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			op_str = ">=";
			break;
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
			op_str = "<=>";
			break;
		default:
			throw NotImplementedException("Unsupported comparison operator in MySQL %s filter", statement);
		}
		return "(" + TransformFilterExpressionToMySQL(*comp.left, column_names, statement) + " " + op_str + " " +
		       TransformFilterExpressionToMySQL(*comp.right, column_names, statement) + ")";
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		string op_str;
		if (conj.type == ExpressionType::CONJUNCTION_AND) {
			op_str = " AND ";
		} else if (conj.type == ExpressionType::CONJUNCTION_OR) {
			op_str = " OR ";
		} else {
			throw NotImplementedException("Unsupported conjunction in MySQL %s filter", statement);
		}
		if (conj.children.empty()) {
			throw NotImplementedException("Empty conjunction in MySQL %s filter", statement);
		}
		vector<string> parts;
		parts.reserve(conj.children.size());
		for (const auto &child : conj.children) {
			parts.push_back(TransformFilterExpressionToMySQL(*child, column_names, statement));
		}
		return "(" + StringUtil::Join(parts, op_str) + ")";
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op_expr = expr.Cast<BoundOperatorExpression>();
		if (op_expr.type == ExpressionType::OPERATOR_IS_NULL && op_expr.children.size() == 1) {
			return "(" + TransformFilterExpressionToMySQL(*op_expr.children[0], column_names, statement) + " IS NULL)";
		}
		if (op_expr.type == ExpressionType::OPERATOR_IS_NOT_NULL && op_expr.children.size() == 1) {
			return "(" + TransformFilterExpressionToMySQL(*op_expr.children[0], column_names, statement) +
			       " IS NOT NULL)";
		}
		if (op_expr.type == ExpressionType::OPERATOR_NOT && op_expr.children.size() == 1) {
			return "(NOT " + TransformFilterExpressionToMySQL(*op_expr.children[0], column_names, statement) + ")";
		}
		if ((op_expr.type == ExpressionType::COMPARE_IN || op_expr.type == ExpressionType::COMPARE_NOT_IN) &&
		    op_expr.children.size() >= 2) {
			string lhs = TransformFilterExpressionToMySQL(*op_expr.children[0], column_names, statement);
			vector<string> items;
			items.reserve(op_expr.children.size() - 1);
			for (idx_t i = 1; i < op_expr.children.size(); i++) {
				items.push_back(TransformFilterExpressionToMySQL(*op_expr.children[i], column_names, statement));
			}
			string neg = op_expr.type == ExpressionType::COMPARE_NOT_IN ? " NOT IN (" : " IN (";
			return "(" + lhs + neg + StringUtil::Join(items, ", ") + "))";
		}
		throw NotImplementedException("Unsupported operator in MySQL %s filter", statement);
	}
	default:
		throw NotImplementedException("Unsupported filter expression in MySQL %s statement", statement);
	}
}

string ExtractFilters(PhysicalOperator &child, const string &statement) {
	// FIXME - all of this is pretty gnarly, we should provide a hook earlier on
	// in the planning process to convert this into a SQL statement
	if (child.type == PhysicalOperatorType::FILTER) {
		auto &filter = child.Cast<PhysicalFilter>();
		auto result = ExtractFilters(child.children[0], statement);
		auto filter_cols = CollectOutputColumnNames(child.children[0]);
		auto filter_str = TransformFilterExpressionToMySQL(*filter.expression, filter_cols, statement);
		if (result.empty()) {
			return filter_str;
		} else {
			return result + " AND " + filter_str;
		}
	} else if (child.type == PhysicalOperatorType::PROJECTION) {
		auto &proj = child.Cast<PhysicalProjection>();
		for (auto &expr : proj.select_list) {
			switch (expr->type) {
			case ExpressionType::BOUND_REF:
			case ExpressionType::BOUND_COLUMN_REF:
			case ExpressionType::VALUE_CONSTANT:
				break;
			default:
				throw NotImplementedException("Unsupported expression type in projection - only simple "
				                              "deletes/updates "
				                              "are supported in the MySQL connector");
			}
		}
		return ExtractFilters(child.children[0], statement);
	} else if (child.type == PhysicalOperatorType::TABLE_SCAN) {
		auto &table_scan = child.Cast<PhysicalTableScan>();
		if (!table_scan.table_filters) {
			return string();
		}
		string result;
		for (auto &entry : table_scan.table_filters->filters) {
			auto column_index = entry.first;
			auto &filter = entry.second;
			if (column_index >= table_scan.column_ids.size()) {
				throw NotImplementedException("Unsupported filter column index in MySQL %s statement", statement);
			}
			const auto col_id = table_scan.column_ids[column_index].GetPrimaryIndex();
			if (col_id == COLUMN_IDENTIFIER_ROW_ID || col_id >= table_scan.names.size()) {
				throw NotImplementedException("Unsupported filter column in MySQL %s statement", statement);
			}
			string escaped_col = MySQLUtils::WriteIdentifier(table_scan.names[col_id]);
			auto filter_str = MySQLFilterPushdown::TransformFilter(escaped_col, *filter);
			if (filter_str.empty()) {
				throw NotImplementedException("Unsupported filter on column \"%s\" in MySQL %s statement",
				                              table_scan.names[col_id], statement);
			}
			if (result.empty()) {
				result = std::move(filter_str);
			} else {
				result += " AND " + filter_str;
			}
		}
		return result;
	} else {
		throw NotImplementedException("Unsupported operator type %s in %s statement - only simple deletes "
		                              "(e.g. %s "
		                              "FROM tbl WHERE x=y) are supported in the MySQL connector",
		                              PhysicalOperatorToString(child.type), statement, statement);
	}
}

string ConstructDeleteStatement(LogicalDelete &op, PhysicalOperator &child) {
	string result = "DELETE FROM ";
	result += MySQLUtils::WriteIdentifier(op.table.schema.name);
	result += ".";
	result += MySQLUtils::WriteIdentifier(op.table.name);
	auto filters = ExtractFilters(child, "DELETE");
	if (!filters.empty()) {
		result += " WHERE " + filters;
	}
	return result;
}

PhysicalOperator &MySQLCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                           PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING clause not yet supported for deletion of a MySQL table");
	}
	MySQLCatalog::MaterializeMySQLScans(plan);

	auto &execute = planner.Make<MySQLExecuteQuery>(op, "DELETE", op.table, ConstructDeleteStatement(op, plan));
	execute.children.push_back(plan);
	return execute;
}

string ConstructUpdateStatement(LogicalUpdate &op, PhysicalOperator &child) {
	// FIXME - all of this is pretty gnarly, we should provide a hook earlier on
	// in the planning process to convert this into a SQL statement
	string result = "UPDATE ";
	result += MySQLUtils::WriteIdentifier(op.table.schema.name);
	result += ".";
	result += MySQLUtils::WriteIdentifier(op.table.name);
	result += " SET ";
	if (child.type != PhysicalOperatorType::PROJECTION) {
		throw NotImplementedException("MySQL Update not supported - Expected the "
		                              "child of an update to be a projection");
	}
	auto &proj = child.Cast<PhysicalProjection>();
	for (idx_t c = 0; c < op.columns.size(); c++) {
		if (c > 0) {
			result += ", ";
		}
		auto &col = op.table.GetColumn(op.table.GetColumns().PhysicalToLogical(op.columns[c]));
		result += MySQLUtils::WriteIdentifier(col.GetName());
		result += " = ";
		if (op.expressions[c]->type == ExpressionType::VALUE_DEFAULT) {
			result += "DEFAULT";
			continue;
		}
		if (op.expressions[c]->type != ExpressionType::BOUND_REF) {
			throw NotImplementedException("MySQL Update not supported - Expected a bound reference expression");
		}
		auto &ref = op.expressions[c]->Cast<BoundReferenceExpression>();
		auto &expr = proj.select_list[ref.index];
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			auto &bound_const_expr = expr->Cast<BoundConstantExpression>();
			result += MySQLUtils::TransformConstant(bound_const_expr.value);
		} else {
			result += expr->ToString();
		}
	}
	result += " ";
	auto filters = ExtractFilters(child.children[0], "UPDATE");
	if (!filters.empty()) {
		result += " WHERE " + filters;
	}
	return result;
}

PhysicalOperator &MySQLCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                           PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING clause not yet supported for updates of a MySQL table");
	}
	MySQLCatalog::MaterializeMySQLScans(plan);

	auto &execute = planner.Make<MySQLExecuteQuery>(op, "UPDATE", op.table, ConstructUpdateStatement(op, plan));
	execute.children.push_back(plan);
	return execute;
}

} // namespace duckdb
