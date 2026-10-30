#include "duckdb.hpp"

#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "mysql_scanner.hpp"
#include "mysql_types.hpp"
#include "mysql_connection_pool.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "storage/mysql_catalog.hpp"

namespace duckdb {

struct ClearCacheFunctionData : public TableFunctionData {
	string catalog;
};

struct ClearCacheState : public GlobalTableFunctionState {
	bool finished = false;
};

static unique_ptr<GlobalTableFunctionState> ClearCacheInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ClearCacheState>();
}

static unique_ptr<FunctionData> ClearCacheBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {

	auto result = make_uniq<ClearCacheFunctionData>();
	if (!input.inputs.empty()) {
		if (input.inputs[0].IsNull() || input.inputs[0].GetValue<string>().empty()) {
			throw InvalidInputException("scoped cache retirement requires a catalog");
		}
		result->catalog = input.inputs[0].GetValue<string>();
		auto &catalog = Catalog::GetCatalog(context, result->catalog);
		if (catalog.GetCatalogType() != "mysql") {
			throw InvalidInputException("scoped cache retirement catalog type mismatch");
		}
	}
	return_types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return std::move(result);
}

static void ClearMySQLCaches(ClientContext &context) {
	MySQLTypeConfig new_config(context);
	auto databases = DatabaseManager::Get(context).GetDatabases(context);
	for (auto &db_ref : databases) {
		auto &db = *db_ref;
		auto &catalog = db.GetCatalog();
		if (catalog.GetCatalogType() != "mysql") {
			continue;
		}
		auto &mysql_catalog = catalog.Cast<MySQLCatalog>();
		mysql_catalog.ClearCache();
		mysql_catalog.GetConnectionPool().UpdateTypeConfig(new_config);
	}
}

static void ClearCacheFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<ClearCacheFunctionData>();
	auto &state = data_p.global_state->Cast<ClearCacheState>();
	if (state.finished) {
		return;
	}
	if (data.catalog.empty()) {
		ClearMySQLCaches(context);
	} else {
		// Resolve one existing attachment only; never enumerate schemas or
		// retire another source's caches. Catalog scope conservatively covers
		// FK dependencies and prefetched schema result slices.
		auto &catalog = Catalog::GetCatalog(context, data.catalog);
		if (catalog.GetCatalogType() != "mysql") {
			throw InvalidInputException("scoped cache retirement catalog type changed");
		}
		catalog.Cast<MySQLCatalog>().ClearCache();
	}
	state.finished = true;
}

void MySQLClearCacheFunction::ClearCacheOnSetting(ClientContext &context, SetScope scope, Value &parameter) {
	ClearMySQLCaches(context);
}

MySQLClearCacheFunction::MySQLClearCacheFunction(bool scoped)
    : TableFunction(scoped ? "mysql_clear_cache_scoped_v1" : "mysql_clear_cache",
                    scoped ? vector<LogicalType>{LogicalType::VARCHAR} : vector<LogicalType>{},
                    ClearCacheFunction, ClearCacheBind, ClearCacheInit) {
}
} // namespace duckdb
