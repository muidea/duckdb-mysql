#include "storage/federation/plan_cache.hpp"
#include <stdexcept>
#include <thread>
#include <iostream>

using namespace duckdb;
static void Require(bool value) {
	if (!value) {
		throw std::runtime_error("plan cache resource assertion");
	}
}
static ExecutionPlanCacheKey Key(idx_t i) {
	return {"schema", "table" + std::to_string(i), {"id"}, {}, {}};
}
int main() {
	auto now = std::chrono::steady_clock::now();
	PlanCache cache([&] { return now; });
	ExecutionPlan plan;
	plan.estimated_rows_from_mysql = 1000;
	CachedExecutionPlan result;
	for (idx_t i = 0; i < 5000; i++) {
		auto key = Key(i);
		cache.CachePlan(key, plan);
		now += std::chrono::seconds(301);
		Require(!cache.GetCachedPlan(key, result));
		Require(cache.AuxiliarySizes().first <= 2 * PlanCache::MAX_PLAN_CACHE_SIZE);
	}
	for (idx_t i = 0; i < 5000; i++) {
		auto key = Key(i);
		auto generation = cache.CachePlan(key, plan);
		Require(!cache.UpdatePlanFeedback(key, 10000, generation + 1));
		for (idx_t j = 0; j < 2; j++) {
			Require(!cache.UpdatePlanFeedback(key, 10000, generation));
		}
		Require(cache.UpdatePlanFeedback(key, 10000, generation));
		Require(cache.AuxiliarySizes().first <= 2000 && cache.AuxiliarySizes().second <= 1000);
	}
	auto key = Key(4999);
	auto generation = cache.CachePlan(key, plan);
	for (idx_t j = 0; j < 5; j++) {
		Require(!cache.UpdatePlanFeedback(key, 10000, generation));
	}
	Require(cache.UpdatePlanFeedback(key, 10000, generation));
	cache.InvalidateTable("schema", "table4999");
	generation = cache.CachePlan(key, plan);
	for (idx_t j = 0; j < 2; j++) {
		Require(!cache.UpdatePlanFeedback(key, 10000, generation));
	}
	Require(cache.UpdatePlanFeedback(key, 10000, generation));
	cache.Clear();
	Require(cache.Size() == 0 && cache.AuxiliarySizes() == std::make_pair(idx_t(0), idx_t(0)));
	std::thread a([&] {
		for (idx_t i = 0; i < 5000; i++) {
			cache.CachePlan(Key(i), plan);
		}
	});
	std::thread b([&] {
		for (idx_t i = 0; i < 5000; i++) {
			CachedExecutionPlan out;
			cache.GetCachedPlan(Key(i), out);
		}
	});
	a.join();
	b.join();
	Require(cache.Size() <= 1000 && cache.AuxiliarySizes().first <= 2000);
	std::cout << "plan cache resource tests passed\n";
}
