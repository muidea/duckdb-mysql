#include "storage/mysql_statistics.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace duckdb;
static void Require(bool value) {
	if (!value) {
		throw std::runtime_error("statistics cache resource assertion");
	}
}
int main() {
	MySQLStatsCache cache;
	MySQLTableStats stats, result;
	stats.estimated_row_count = 123;
	for (idx_t i = 0; i < 10000; i++) {
		auto table = "table" + std::to_string(i);
		cache.StoreTableStats("schema", table, stats);
		Require(cache.GetTableStats("schema", table, result) && result.estimated_row_count == 123);
		cache.Invalidate("schema", table);
		Require(cache.AuxiliaryQueueSize() <= 2 * MySQLStatsCache::MAX_TABLE_STATS_ENTRIES);
	}
	for (idx_t i = 0; i < 10000; i++) {
		cache.StoreTableStats("schema", "same", stats);
		Require(cache.AuxiliaryQueueSize() <= 2 * MySQLStatsCache::MAX_TABLE_STATS_ENTRIES);
	}
	cache.Clear();
	Require(cache.AuxiliaryQueueSize() == 0);
	std::thread writer([&] {
		for (idx_t i = 0; i < 5000; i++) {
			cache.StoreTableStats("schema", std::to_string(i), stats);
		}
	});
	std::thread reader([&] {
		for (idx_t i = 0; i < 5000; i++) {
			MySQLTableStats out;
			cache.GetTableStats("schema", std::to_string(i), out);
			cache.Invalidate("schema", std::to_string(i));
		}
	});
	writer.join();
	reader.join();
	Require(cache.AuxiliaryQueueSize() <= 2 * MySQLStatsCache::MAX_TABLE_STATS_ENTRIES);
	cache.Clear();
	Require(cache.AuxiliaryQueueSize() == 0);
	std::cout << "statistics cache resource tests passed\n";
}
