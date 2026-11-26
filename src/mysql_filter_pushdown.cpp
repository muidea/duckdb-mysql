#include "mysql_filter_pushdown.hpp"
#include "mysql_utils.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"

namespace duckdb {

string MySQLFilterPushdown::CreateExpression(string &column_name, vector<unique_ptr<TableFilter>> &filters, string op) {
	vector<string> filter_entries;
	for (auto &filter : filters) {
		auto new_filter = TransformFilter(column_name, *filter);
		if (new_filter.empty()) {
			return string();
		}
		filter_entries.push_back(std::move(new_filter));
	}
	if (filter_entries.empty()) {
		return string();
	}
	return "(" + StringUtil::Join(filter_entries, " " + op + " ") + ")";
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
	switch (filter.filter_type) {
	case TableFilterType::IS_NULL:
		return column_name + " IS NULL";
	case TableFilterType::IS_NOT_NULL:
		return column_name + " IS NOT NULL";
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction_filter = filter.Cast<ConjunctionAndFilter>();
		return CreateExpression(column_name, conjunction_filter.child_filters, "AND");
	}
	case TableFilterType::CONJUNCTION_OR: {
		auto &conjunction_filter = filter.Cast<ConjunctionOrFilter>();
		return CreateExpression(column_name, conjunction_filter.child_filters, "OR");
	}
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant_filter = filter.Cast<ConstantFilter>();
		string constant_string;
		if (!MySQLUtils::TryTransformConstant(constant_filter.constant, constant_string)) {
			return string();
		}
		auto operator_string = TransformComparison(constant_filter.comparison_type);
		return StringUtil::Format("%s %s %s", column_name, operator_string, constant_string);
	}
	case TableFilterType::OPTIONAL_FILTER: {
		auto &optional_filter = filter.Cast<OptionalFilter>();
		return TransformFilter(column_name, *optional_filter.child_filter);
	}
	case TableFilterType::DYNAMIC_FILTER: {
		return string();
	}
	case TableFilterType::IN_FILTER: {
		auto &in_filter = filter.Cast<InFilter>();
		if (in_filter.values.empty()) {
			return string();
		}
		string in_list;
		for (auto &val : in_filter.values) {
			string constant_string;
			if (!MySQLUtils::TryTransformConstant(val, constant_string)) {
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
		throw InternalException("Unsupported table filter type");
	}
}

string MySQLFilterPushdown::TransformFilters(const vector<column_t> &column_ids, optional_ptr<TableFilterSet> filters,
                                             const vector<string> &names, vector<column_t> *unpushed_columns) {
	if (!filters || filters->filters.empty()) {
		// no filters
		return string();
	}
	string result;
	for (auto &entry : filters->filters) {
		auto col_idx = column_ids[entry.first];
		if (col_idx == COLUMN_IDENTIFIER_ROW_ID || col_idx >= names.size()) {
			if (unpushed_columns) {
				unpushed_columns->push_back(entry.first);
			}
			continue;
		}
		auto column_name = MySQLUtils::WriteIdentifier(names[col_idx]);
		auto &filter = *entry.second;
		auto new_filter = TransformFilter(column_name, filter);
		if (new_filter.empty()) {
			if (unpushed_columns) {
				unpushed_columns->push_back(entry.first);
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
