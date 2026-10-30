//===----------------------------------------------------------------------===//
//                         DuckDB
//
// mysql_scanner.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

#include "mysql_connection.hpp"
#include "mysql_connection_pool.hpp"
#include "mysql_statement.hpp"
#include "mysql_types.hpp"
#include "mysql_utils.hpp"

namespace duckdb {
class MySQLTableEntry;
class MySQLTransaction;

struct MySQLBindData : public FunctionData {
	explicit MySQLBindData(MySQLTableEntry &table) : table(table) {
	}

	MySQLTableEntry &table;
	vector<MySQLType> mysql_types;
	vector<string> names;
	vector<LogicalType> types;
	string limit;
	string order_by_clause;
	string aggregate_select_list;
	string group_by_clause;
	string aggregate_where_clause;
	bool has_aggregate_pushdown = false;
	MySQLResultStreaming streaming = MySQLResultStreaming::UNINITIALIZED;

	bool use_predicate_analyzer = false;

public:
	unique_ptr<FunctionData> Copy() const override {
		throw NotImplementedException("MySQLBindData copy not supported");
	}
	bool Equals(const FunctionData &other_p) const override {
		return false;
	}
};

struct MySQLQueryBindData : public FunctionData {
	MySQLQueryBindData(string query_p, Catalog &catalog, vector<Value> params_p,
	                   vector<LogicalType> types_p, vector<string> names_p, bool text_protocol = false,
	                   bool materialize = false)
	    : query(std::move(query_p)), catalog(catalog), params(std::move(params_p)),
	      types(std::move(types_p)), names(std::move(names_p)), use_text_protocol(text_protocol),
	      materialized(materialize) {
	}

	string query;
	Catalog &catalog;
	vector<Value> params;
	vector<LogicalType> types;
	vector<string> names;
	bool use_text_protocol = false;
	bool materialized = false;

public:
	unique_ptr<FunctionData> Copy() const override {
		throw NotImplementedException("MySQLBindData copy not supported");
	}
	bool Equals(const FunctionData &other_p) const override {
		return false;
	}
};

class MySQLScanFunction : public TableFunction {
public:
	MySQLScanFunction();
};

class MySQLQueryFunction : public TableFunction {
public:
	MySQLQueryFunction();
};

class MySQLClearCacheFunction : public TableFunction {
public:
	explicit MySQLClearCacheFunction(bool scoped = false);

	static void ClearCacheOnSetting(ClientContext &context, SetScope scope, Value &parameter);
};

class MySQLExecuteFunction : public TableFunction {
public:
	MySQLExecuteFunction();
};

class MySQLExplainFederatedFunction : public TableFunction {
public:
	MySQLExplainFederatedFunction();
};

class MySQLDebugExecutionPlanFunction : public TableFunction {
public:
	MySQLDebugExecutionPlanFunction();
};

} // namespace duckdb
