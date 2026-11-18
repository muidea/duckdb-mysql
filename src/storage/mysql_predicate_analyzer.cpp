#include "storage/mysql_predicate_analyzer.hpp"
#include "mysql_utils.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

#include <cmath>
#include <unordered_set>

namespace duckdb {

PredicateAnalyzer::PredicateAnalyzer(MySQLStatisticsCollector &stats, const string &schema, const string &table)
    : stats_(stats), schema_(schema), table_(table) {
	table_stats_ = stats_.get().GetTableStats(schema, table);
}

void PredicateAnalyzer::SetHintInjectionEnabled(bool enabled, double staleness_threshold) {
	hint_injection_enabled_ = enabled;
	hint_staleness_threshold_ = staleness_threshold;
}

void PredicateAnalyzer::SetPushThresholds(double with_index, double no_index) {
	push_threshold_with_index_ = with_index;
	push_threshold_no_index_ = no_index;
}

static bool IsDirectColumnRefExpr(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_REF ||
	       expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF;
}

static const Expression *UnwrapOptionalFilterExpr(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	if (func.Function().GetName() == OptionalFilterScalarFun::NAME && func.BindInfo()) {
		return func.BindInfo()->Cast<OptionalFilterFunctionData>().child_filter_expr.get();
	}
	if (func.Function().GetName() == SelectivityOptionalFilterScalarFun::NAME && func.BindInfo()) {
		return func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>().child_filter_expr.get();
	}
	return nullptr;
}

static bool ExtractComparisonConstant(const Expression &expr, ExpressionType &cmp_type, const Value *&constant) {
	if (!BoundComparisonExpression::IsComparison(expr)) {
		return false;
	}
	auto &cmp = expr.Cast<BoundFunctionExpression>();
	cmp_type = cmp.GetExpressionType();
	auto &left = BoundComparisonExpression::Left(cmp);
	auto &right = BoundComparisonExpression::Right(cmp);
	if (IsDirectColumnRefExpr(left) && right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		constant = &right.Cast<BoundConstantExpression>().GetValue();
		return true;
	}
	if (left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT && IsDirectColumnRefExpr(right)) {
		constant = &left.Cast<BoundConstantExpression>().GetValue();
		cmp_type = FlipComparisonExpression(cmp_type);
		return true;
	}
	return false;
}

static bool IsPushableFilterExpression(const Expression &expr) {
	ExpressionType cmp_type;
	const Value *constant = nullptr;
	if (ExtractComparisonConstant(expr, cmp_type, constant)) {
		return true;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		for (const auto &child : conj.GetChildren()) {
			if (!IsPushableFilterExpression(*child)) {
				return false;
			}
		}
		return !conj.GetChildren().empty();
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		auto &children = op.GetChildren();
		if (children.empty() || !IsDirectColumnRefExpr(*children[0])) {
			return false;
		}
		switch (op.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			return true;
		case ExpressionType::COMPARE_IN:
			for (idx_t i = 1; i < children.size(); i++) {
				if (children[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
					return false;
				}
			}
			return children.size() > 1;
		default:
			return false;
		}
	}
	case ExpressionClass::BOUND_FUNCTION: {
		if (const auto *inner = UnwrapOptionalFilterExpr(expr)) {
			return IsPushableFilterExpression(*inner);
		}
		return false;
	}
	default:
		return false;
	}
}

bool PredicateAnalyzer::IsPushableFilterType(TableFilterType type) const {
	return type == TableFilterType::EXPRESSION_FILTER;
}

bool PredicateAnalyzer::CanPushFilter(TableFilter &filter) const {
	if (filter.filter_type != TableFilterType::EXPRESSION_FILTER) {
		return false;
	}
	auto &ef = filter.Cast<ExpressionFilter>();
	return ef.expr && IsPushableFilterExpression(*ef.expr);
}

string PredicateAnalyzer::TransformComparisonOperator(ExpressionType type) const {
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
		return "";
	}
}

string PredicateAnalyzer::CreateConjunction(const vector<string> &predicates, const string &op) const {
	if (predicates.empty()) {
		return "";
	}
	if (predicates.size() == 1) {
		return predicates[0];
	}
	return "(" + StringUtil::Join(predicates, " " + op + " ") + ")";
}

static string TransformExprToMySQL(const string &col_escaped, const Expression &expr,
                                   const std::function<string(ExpressionType)> &cmp_op_fn) {
	ExpressionType cmp_type;
	const Value *constant = nullptr;
	if (ExtractComparisonConstant(expr, cmp_type, constant)) {
		auto constant_string = MySQLUtils::TransformConstant(*constant);
		auto operator_string = cmp_op_fn(cmp_type);
		if (operator_string.empty()) {
			return "";
		}
		return StringUtil::Format("%s %s %s", col_escaped, operator_string, constant_string);
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto op = conj.GetExpressionType() == ExpressionType::CONJUNCTION_AND ? "AND" : "OR";
		vector<string> parts;
		for (const auto &child : conj.GetChildren()) {
			auto pred = TransformExprToMySQL(col_escaped, *child, cmp_op_fn);
			if (!pred.empty()) {
				parts.push_back(std::move(pred));
			}
		}
		if (parts.empty()) {
			return "";
		}
		if (parts.size() == 1) {
			return parts[0];
		}
		return "(" + StringUtil::Join(parts, " " + string(op) + " ") + ")";
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		auto &children = op.GetChildren();
		if (children.empty() || !IsDirectColumnRefExpr(*children[0])) {
			return "";
		}
		switch (op.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
			return col_escaped + " IS NULL";
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			return col_escaped + " IS NOT NULL";
		case ExpressionType::COMPARE_IN: {
			if (children.size() <= 1) {
				return "";
			}
			string in_list;
			for (idx_t i = 1; i < children.size(); i++) {
				if (children[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
					return "";
				}
				if (!in_list.empty()) {
					in_list += ", ";
				}
				in_list += MySQLUtils::TransformConstant(children[i]->Cast<BoundConstantExpression>().GetValue());
			}
			return col_escaped + " IN (" + in_list + ")";
		}
		default:
			return "";
		}
	}
	case ExpressionClass::BOUND_FUNCTION: {
		if (const auto *inner = UnwrapOptionalFilterExpr(expr)) {
			return TransformExprToMySQL(col_escaped, *inner, cmp_op_fn);
		}
		return "";
	}
	default:
		return "";
	}
}

string PredicateAnalyzer::TransformFilterToMySQL(const string &column_name, TableFilter &filter) const {
	string col_escaped = MySQLUtils::WriteIdentifier(column_name);
	if (filter.filter_type != TableFilterType::EXPRESSION_FILTER) {
		return "";
	}
	auto &ef = filter.Cast<ExpressionFilter>();
	if (!ef.expr) {
		return "";
	}
	return TransformExprToMySQL(col_escaped, *ef.expr,
	                            [this](ExpressionType type) { return TransformComparisonOperator(type); });
}

static double EstimateExprSelectivity(MySQLStatisticsCollector &stats, const MySQLTableStats &table_stats,
                                      const string &schema, const string &table, const string &column_name,
                                      const Expression &expr) {
	ExpressionType cmp_type;
	const Value *constant = nullptr;
	if (ExtractComparisonConstant(expr, cmp_type, constant)) {
		return stats.EstimateSelectivity(schema, table, column_name, cmp_type, *constant);
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		if (conj.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
			double selectivity = 1.0;
			for (const auto &child : conj.GetChildren()) {
				selectivity *= EstimateExprSelectivity(stats, table_stats, schema, table, column_name, *child);
			}
			return selectivity;
		}
		double combined = 1.0;
		for (const auto &child : conj.GetChildren()) {
			double child_sel = EstimateExprSelectivity(stats, table_stats, schema, table, column_name, *child);
			combined *= (1.0 - child_sel);
		}
		return 1.0 - combined;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		switch (op.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL: {
			auto it = table_stats.column_null_fraction.find(column_name);
			if (it != table_stats.column_null_fraction.end()) {
				return it->second;
			}
			return 0.01;
		}
		case ExpressionType::OPERATOR_IS_NOT_NULL: {
			auto it = table_stats.column_null_fraction.find(column_name);
			if (it != table_stats.column_null_fraction.end()) {
				return 1.0 - it->second;
			}
			return 0.99;
		}
		case ExpressionType::COMPARE_IN: {
			idx_t value_count = op.GetChildren().size() > 1 ? op.GetChildren().size() - 1 : 0;
			auto it = table_stats.column_distinct_count.find(column_name);
			if (it != table_stats.column_distinct_count.end() && it->second > 0) {
				return std::min(static_cast<double>(value_count) / static_cast<double>(it->second), 1.0);
			}
			double per_value_selectivity = value_count > 10 ? 0.005 : 0.01;
			return std::min(static_cast<double>(value_count) * per_value_selectivity, 0.8);
		}
		default:
			return PredicateAnalyzer::DEFAULT_SELECTIVITY;
		}
	}
	case ExpressionClass::BOUND_FUNCTION: {
		if (const auto *inner = UnwrapOptionalFilterExpr(expr)) {
			return EstimateExprSelectivity(stats, table_stats, schema, table, column_name, *inner);
		}
		return PredicateAnalyzer::DEFAULT_SELECTIVITY;
	}
	default:
		return PredicateAnalyzer::DEFAULT_SELECTIVITY;
	}
}

double PredicateAnalyzer::EstimateFilterSelectivity(const string &column_name, TableFilter &filter) {
	if (filter.filter_type != TableFilterType::EXPRESSION_FILTER) {
		return DEFAULT_SELECTIVITY;
	}
	auto &ef = filter.Cast<ExpressionFilter>();
	if (!ef.expr) {
		return DEFAULT_SELECTIVITY;
	}
	return EstimateExprSelectivity(stats_.get(), table_stats_, schema_, table_, column_name, *ef.expr);
}

PushdownDecision PredicateAnalyzer::MakePushdownDecision(double selectivity, bool has_index) const {
	if (has_index && selectivity < push_threshold_with_index_) {
		return PushdownDecision::PUSH_TO_MYSQL;
	}
	if (!has_index && selectivity < push_threshold_no_index_) {
		return PushdownDecision::PUSH_TO_MYSQL;
	}
	return PushdownDecision::EXECUTE_IN_DUCKDB;
}

#ifndef NDEBUG
string PredicateAnalyzer::GenerateReasoning(double selectivity, bool has_index, PushdownDecision decision) const {
	string reason;
	reason += "selectivity=" + std::to_string(selectivity);
	reason += ", has_index=" + string(has_index ? "true" : "false");
	reason += ", decision=";
	switch (decision) {
	case PushdownDecision::PUSH_TO_MYSQL:
		reason += "PUSH";
		break;
	case PushdownDecision::EXECUTE_IN_DUCKDB:
		reason += "LOCAL";
		break;
	case PushdownDecision::PUSH_PARTIAL:
		reason += "PARTIAL";
		break;
	}
	return reason;
}
#endif

PredicateAnalysis PredicateAnalyzer::AnalyzeFilter(const string &column_name, TableFilter &filter) {
	PredicateAnalysis result;
	result.column_name = column_name;

	if (!CanPushFilter(filter)) {
		result.decision = PushdownDecision::EXECUTE_IN_DUCKDB;
		result.estimated_selectivity = DEFAULT_SELECTIVITY;
#ifndef NDEBUG
		result.reasoning = "unpushable filter type";
#endif
		return result;
	}

	result.has_index = table_stats_.HasIndexOnColumn(column_name);
	result.is_partition_key = table_stats_.partition_info.IsPartitionColumn(column_name);
	result.estimated_selectivity = EstimateFilterSelectivity(column_name, filter);

	if (result.is_partition_key) {
		result.decision = PushdownDecision::PUSH_TO_MYSQL;
	} else {
		result.decision = MakePushdownDecision(result.estimated_selectivity, result.has_index);
	}

	result.mysql_predicate = TransformFilterToMySQL(column_name, filter);
	if (result.mysql_predicate.empty()) {
		result.decision = PushdownDecision::EXECUTE_IN_DUCKDB;
	}

#ifndef NDEBUG
	result.reasoning = GenerateReasoning(result.estimated_selectivity, result.has_index, result.decision);
	if (result.is_partition_key) {
		result.reasoning += ", partition_key=true (force push)";
	}
#endif

	return result;
}

void PredicateAnalyzer::ReorderPredicatesForIndex(vector<PredicateAnalysis> &analyses,
                                                  vector<string> &pushed_predicates) const {
	if (pushed_predicates.size() < 2 || table_stats_.indexes.empty()) {
		return;
	}

	vector<string> filter_columns;
	for (const auto &analysis : analyses) {
		if (analysis.ShouldPush() && !analysis.mysql_predicate.empty()) {
			filter_columns.push_back(analysis.column_name);
		}
	}

	if (filter_columns.size() < 2) {
		return;
	}

	optional_ptr<const MySQLIndexInfo> best_index = table_stats_.FindBestIndex(filter_columns);
	if (!best_index) {
		for (const auto &index : table_stats_.indexes) {
			idx_t matched = 0;
			for (const auto &col : filter_columns) {
				for (const auto &idx_col : index.columns) {
					if (StringUtil::CIEquals(col, idx_col)) {
						matched++;
						break;
					}
				}
			}
			if (matched >= 2 && (!best_index || matched > filter_columns.size() / 2)) {
				best_index = &index;
			}
		}
	}

	if (!best_index || best_index->columns.size() < 2) {
		return;
	}

	case_insensitive_map_t<vector<idx_t>> column_to_analysis_indices;
	for (idx_t i = 0; i < analyses.size(); i++) {
		if (analyses[i].ShouldPush() && !analyses[i].mysql_predicate.empty()) {
			column_to_analysis_indices[analyses[i].column_name].push_back(i);
		}
	}

	vector<string> reordered_predicates;
	unordered_set<idx_t> used_indices;

	for (const auto &idx_col : best_index->columns) {
		auto it = column_to_analysis_indices.find(idx_col);
		if (it != column_to_analysis_indices.end()) {
			for (auto analysis_idx : it->second) {
				if (used_indices.find(analysis_idx) == used_indices.end()) {
					reordered_predicates.push_back(analyses[analysis_idx].mysql_predicate);
					used_indices.insert(analysis_idx);
				}
			}
		}
	}

	for (idx_t i = 0; i < analyses.size(); i++) {
		if (analyses[i].ShouldPush() && !analyses[i].mysql_predicate.empty() &&
		    used_indices.find(i) == used_indices.end()) {
			reordered_predicates.push_back(analyses[i].mysql_predicate);
		}
	}

	if (reordered_predicates.size() == pushed_predicates.size()) {
		pushed_predicates = std::move(reordered_predicates);
	}
}

FilterAnalysisResult PredicateAnalyzer::AnalyzeFilters(const vector<column_t> &column_ids,
                                                       optional_ptr<TableFilterSet> filters,
                                                       const vector<string> &names) {
	FilterAnalysisResult result;

	if (!filters || !filters->HasFilters()) {
		return result;
	}

	vector<string> pushed_predicates;

	idx_t filter_count = 0;
	for (auto &entry : *filters) {
		column_t col_idx = entry.GetIndex().GetIndexUnsafe();
		if (col_idx >= column_ids.size()) {
			continue;
		}
		column_t actual_col_idx = column_ids[col_idx];
		if (actual_col_idx >= names.size()) {
			continue;
		}
		string column_name = names[actual_col_idx];
		auto &filter = entry.Filter();

		auto analysis = AnalyzeFilter(column_name, filter);
		analysis.column_index = col_idx;
		filter_count++;
		if (filter_count <= 2) {
			result.combined_selectivity *= analysis.estimated_selectivity;
		} else {
			result.combined_selectivity *= std::pow(analysis.estimated_selectivity, SELECTIVITY_DAMPENING_EXPONENT);
		}

		if (analysis.ShouldPush() && !analysis.mysql_predicate.empty()) {
			pushed_predicates.push_back(analysis.mysql_predicate);
			result.filters_pushed++;
		} else {
			result.filters_local++;
		}

		result.analyses.push_back(std::move(analysis));
	}

	if (pushed_predicates.size() >= 2) {
		ReorderPredicatesForIndex(result.analyses, pushed_predicates);
	}

	if (!pushed_predicates.empty()) {
		result.combined_mysql_predicate = StringUtil::Join(pushed_predicates, " AND ");
	}

	if (hint_injection_enabled_) {
		RecommendIndexHint(result);
	}

	return result;
}

void PredicateAnalyzer::RecommendIndexHint(FilterAnalysisResult &result) const {
	if (table_stats_.staleness_score < hint_staleness_threshold_) {
		return;
	}

	vector<string> filter_columns;
	for (const auto &analysis : result.analyses) {
		if (analysis.ShouldPush() && analysis.has_index) {
			filter_columns.push_back(analysis.column_name);
		}
	}

	if (filter_columns.empty()) {
		return;
	}

	auto best_index = table_stats_.FindBestIndex(filter_columns);
	if (!best_index) {
		return;
	}

	result.recommended_index = best_index->index_name;
	result.suggest_force_index = (table_stats_.staleness_score >= FORCE_INDEX_STALENESS_THRESHOLD);
}

} // namespace duckdb
