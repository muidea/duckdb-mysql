#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "mysql_filter_pushdown.hpp"
#include "mysql_parameter.hpp"
#include "mysql_temporal_utils.hpp"
#include "mysql_utils.hpp"
#include "storage/federation/cost_model.hpp"
#include "storage/mysql_statistics.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace duckdb;

static void Require(bool value, const char *msg) {
	if (!value) {
		throw std::runtime_error(msg);
	}
}

template <typename Fn>
static void RequireConversionError(Fn &&fn, const char *msg) {
	bool threw = false;
	try {
		fn();
	} catch (const ConversionException &) {
		threw = true;
	}
	Require(threw, msg);
}

static unique_ptr<Expression> MakeColComparison(ExpressionType cmp_type, const Value &val) {
	auto col_ref = make_uniq<BoundReferenceExpression>(val.type(), 0ULL);
	return BoundComparisonExpression::Create(cmp_type, std::move(col_ref), make_uniq<BoundConstantExpression>(val));
}

int main() {
	// 1. IsAllowedMySQLSessionTimeZone
	for (const char *ok : {"", " ", "+00:00", " +00:00 ", "-00:00", "00:00", "UTC", "utc", "Etc/UTC", "GMT", "Z"}) {
		Require(IsAllowedMySQLSessionTimeZone(ok), "expected allowed UTC session timezone");
	}
	for (const char *bad : {"+08:00", "-05:00", "Asia/Shanghai", "SYSTEM", "America/New_York", "EST"}) {
		Require(!IsAllowedMySQLSessionTimeZone(bad), "expected rejected non-UTC session timezone");
	}

	// 2. MySQLUtils::TryTransformConstant & TransformConstant boundaries
	string out;
	Value d_ad = Value::DATE(Date::FromDate(2026, 1, 2));
	Require(MySQLUtils::TryTransformConstant(d_ad, out) && out == "'2026-01-02'", "AD DATE literal mismatch");

	Value d_bc = Value::DATE(Date::FromDate(-1, 1, 1));
	Require(!MySQLUtils::TryTransformConstant(d_bc, out), "BC DATE should not push down");
	RequireConversionError([&] { MySQLUtils::TransformConstant(d_bc); }, "BC DATE TransformConstant should throw");

	Value d_inf = Value::DATE(date_t::infinity());
	Require(!MySQLUtils::TryTransformConstant(d_inf, out), "infinity DATE should not push down");

	Value ts_ad = Value::TIMESTAMP(Timestamp::FromDatetime(Date::FromDate(2026, 1, 2), Time::FromTime(3, 4, 5, 123456)));
	Require(MySQLUtils::TryTransformConstant(ts_ad, out) && out == "'2026-01-02 03:04:05.123456'",
	        "AD TIMESTAMP literal mismatch");

	Value ts_bc = Value::TIMESTAMP(Timestamp::FromDatetime(Date::FromDate(0, 1, 1), Time::FromTime(12, 0, 0, 0)));
	Require(!MySQLUtils::TryTransformConstant(ts_bc, out), "BC TIMESTAMP should not push down");

	Value ts_inf = Value::TIMESTAMP(timestamp_t::infinity());
	Require(!MySQLUtils::TryTransformConstant(ts_inf, out), "infinity TIMESTAMP should not push down");

	// TIMESTAMP_TZ range boundaries (MySQL TIMESTAMP valid UTC range: 1970-01-01 00:00:01 .. 2038-01-19 03:14:07.999999 UTC)
	Value tstz_1970_0 = Value::TIMESTAMPTZ(
	    timestamp_tz_t(Timestamp::FromDatetime(Date::FromDate(1970, 1, 1), Time::FromTime(0, 0, 0, 0))));
	Value tstz_1970_1 = Value::TIMESTAMPTZ(
	    timestamp_tz_t(Timestamp::FromDatetime(Date::FromDate(1970, 1, 1), Time::FromTime(0, 0, 1, 0))));
	Value tstz_2038_max = Value::TIMESTAMPTZ(
	    timestamp_tz_t(Timestamp::FromDatetime(Date::FromDate(2038, 1, 19), Time::FromTime(3, 14, 7, 999999))));
	Value tstz_2038_over = Value::TIMESTAMPTZ(
	    timestamp_tz_t(Timestamp::FromDatetime(Date::FromDate(2038, 1, 19), Time::FromTime(3, 14, 8, 0))));

	Require(!MySQLUtils::TryTransformConstant(tstz_1970_0, out), "1970-01-01 00:00:00+00 should not push down");
	Require(MySQLUtils::TryTransformConstant(tstz_1970_1, out) && out == "'1970-01-01 00:00:01'",
	        "1970-01-01 00:00:01+00 literal mismatch");
	Require(MySQLUtils::TryTransformConstant(tstz_2038_max, out) && out == "'2038-01-19 03:14:07.999999'",
	        "2038-01-19 03:14:07.999999+00 literal mismatch");
	Require(!MySQLUtils::TryTransformConstant(tstz_2038_over, out), "2038-01-19 03:14:08+00 should not push down");
	RequireConversionError([&] { MySQLUtils::TransformConstant(tstz_2038_over); },
	                       "out-of-range TIMESTAMPTZ TransformConstant should throw");

	// 3. MySQLParameter binary bindings (no std::abs corruption)
	{
		MySQLParameter p_date("q", d_ad);
		Require(p_date.bind_buffer.size() == sizeof(MYSQL_TIME), "DATE param buffer size");
		MYSQL_TIME mt {};
		std::memcpy(&mt, p_date.bind_buffer.data(), sizeof(MYSQL_TIME));
		Require(mt.year == 2026 && mt.month == 1 && mt.day == 2, "AD DATE param MYSQL_TIME mismatch");

		MySQLParameter p_tstz("q", tstz_2038_max);
		std::memcpy(&mt, p_tstz.bind_buffer.data(), sizeof(MYSQL_TIME));
		Require(mt.year == 2038 && mt.month == 1 && mt.day == 19 && mt.hour == 3 && mt.minute == 14 && mt.second == 7 &&
		            mt.second_part == 999999,
		        "TIMESTAMPTZ param MYSQL_TIME mismatch");
	}
	RequireConversionError([&] { MySQLParameter p("q", d_bc); }, "BC DATE parameter should throw");
	RequireConversionError([&] { MySQLParameter p("q", d_inf); }, "infinity DATE parameter should throw");
	RequireConversionError([&] { MySQLParameter p("q", ts_bc); }, "BC TIMESTAMP parameter should throw");
	RequireConversionError([&] { MySQLParameter p("q", ts_inf); }, "infinity TIMESTAMP parameter should throw");
	RequireConversionError([&] { MySQLParameter p("q", tstz_1970_0); }, "1970-01-01 00:00:00 TIMESTAMPTZ param throw");
	RequireConversionError([&] { MySQLParameter p("q", tstz_2038_over); }, "2038-01-19 03:14:08 TIMESTAMPTZ param throw");

	// 4. MySQLFilterPushdown::TransformFilter clean SQL vs safe empty fallback (DuckDB 2.0 ExpressionFilter)
	{
		string col = "`ts`";
		auto cf_ok = ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_EQUAL, tstz_1970_1);
		Require(MySQLFilterPushdown::TransformFilter(col, *cf_ok) == "`ts` = '1970-01-01 00:00:01'",
		        "MySQLFilterPushdown valid TIMESTAMPTZ mismatch");

		auto cf_oob = ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHAN, d_bc);
		Require(MySQLFilterPushdown::TransformFilter(col, *cf_oob).empty(), "BC filter must return empty for local eval");

		auto in_expr = ExpressionFilter::CreateInExpression(make_uniq<BoundReferenceExpression>(d_ad.type(), 0ULL),
		                                                    vector<Value> {d_ad, d_bc});
		ExpressionFilter inf_mixed(std::move(in_expr));
		Require(MySQLFilterPushdown::TransformFilter(col, inf_mixed).empty(),
		        "IN filter containing unpushable constant must return empty");

		auto or_conj = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
		or_conj->GetChildrenMutable().push_back(MakeColComparison(ExpressionType::COMPARE_EQUAL, tstz_1970_1));
		or_conj->GetChildrenMutable().push_back(MakeColComparison(ExpressionType::COMPARE_EQUAL, tstz_2038_over));
		ExpressionFilter or_mixed(std::move(or_conj));
		Require(MySQLFilterPushdown::TransformFilter(col, or_mixed).empty(),
		        "OR filter with unpushable branch must return empty");

		auto and_conj = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		and_conj->GetChildrenMutable().push_back(MakeColComparison(ExpressionType::COMPARE_EQUAL, tstz_1970_1));
		and_conj->GetChildrenMutable().push_back(MakeColComparison(ExpressionType::COMPARE_LESSTHAN, tstz_2038_over));
		ExpressionFilter and_mixed(std::move(and_conj));
		Require(MySQLFilterPushdown::TransformFilter(col, and_mixed).empty(),
		        "AND filter with unpushable branch must return empty");

		TableFilterSet filter_set;
		filter_set.PushFilter(ProjectionIndex(0),
		                      ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_EQUAL, tstz_1970_1));
		filter_set.PushFilter(ProjectionIndex(1),
		                      ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHAN, d_bc));
		vector<column_t> unpushed_columns;
		auto pushed_sql = MySQLFilterPushdown::TransformFilters({0, 1}, &filter_set, {"ts", "d"}, &unpushed_columns);
		Require(pushed_sql == "`ts` = '1970-01-01 00:00:01'", "TransformFilters pushed SQL mismatch");
		Require(unpushed_columns.size() == 1 && unpushed_columns[0] == 1,
		        "TransformFilters unpushed_columns mismatch");
	}

	// 5. MySQLPartitionInfo::GetMatchingPartitions RANGE_COLUMNS quoted date & TIMESTAMP_TZ/infinity safety
	{
		auto make_part = [](const char *name, const char *desc) {
			MySQLPartitionDetail d;
			d.partition_name = name;
			d.estimated_rows = 10;
			d.is_subpartition = false;
			d.description = desc;
			return d;
		};
		MySQLPartitionInfo pinfo;
		pinfo.type = MySQLPartitionType::RANGE_COLUMNS;
		pinfo.partition_columns.push_back("d");
		pinfo.partitions.push_back(make_part("p0", "'2026-01-01'"));
		pinfo.partitions.push_back(make_part("p1", "'2026-02-01'"));
		pinfo.partitions.push_back(make_part("pmax", "MAXVALUE"));
		auto parts = pinfo.GetMatchingPartitions(Value::DATE(Date::FromDate(2026, 1, 15)), ExpressionType::COMPARE_EQUAL);
		Require(parts.size() == 1 && parts[0] == "p1", "RANGE_COLUMNS quoted DATE pruning mismatch");

		Require(pinfo.GetMatchingPartitions(tstz_1970_1, ExpressionType::COMPARE_EQUAL).empty(),
		        "TIMESTAMP_TZ must disable partition pruning");
		Require(pinfo.GetMatchingPartitions(d_inf, ExpressionType::COMPARE_EQUAL).empty(),
		        "infinity DATE must disable partition pruning");

		pinfo.partitions.clear();
		pinfo.partitions.push_back(make_part("p_multi", "'2026-01-01',10"));
		Require(pinfo.GetMatchingPartitions(d_ad, ExpressionType::COMPARE_EQUAL).empty(),
		        "multi-column RANGE_COLUMNS must return empty");
	}

	// 6. DefaultCostModel::ComparePlans never chooses PUSH_ALL_FILTERS when any predicate is empty
	{
		MySQLTableStats stats;
		stats.estimated_row_count = 1000;
		stats.avg_row_length = 64;

		FilterAnalysisResult far;
		PredicateAnalysis pa_local;
		pa_local.column_name = "d";
		pa_local.column_index = 0;
		pa_local.decision = PushdownDecision::EXECUTE_IN_DUCKDB;
		pa_local.mysql_predicate = "";
		pa_local.estimated_selectivity = 0.01;
		far.analyses.push_back(pa_local);
		far.combined_selectivity = 0.01;

		DefaultCostModel model;
		auto plan = model.ComparePlans(stats, far, {"d"});
		Require(plan.strategy == ExecutionStrategy::EXECUTE_ALL_LOCALLY,
		        "unpushable predicate must select EXECUTE_ALL_LOCALLY");
		Require(plan.pushed_filter_indices.empty() && plan.local_filter_indices.size() == 1,
		        "unpushable predicate must land in local_filter_indices");
	}

	std::cout << "MySQL temporal behavior tests passed\n";
	return 0;
}
