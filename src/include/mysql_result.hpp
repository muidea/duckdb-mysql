//===----------------------------------------------------------------------===//
//                         DuckDB
//
// mysql_result.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "mysql_field.hpp"
#include "mysql_statement.hpp"
#include "mysql_types.hpp"
#include "mysql_utils.hpp"

namespace duckdb {
class MySQLConnection;
struct OwnedMySQLConnection;

using MySQLResultPtr = duckdb::unique_ptr<MYSQL_RES, void (*)(MYSQL_RES *)>;

inline void MySQLResultDelete(MYSQL_RES *res) {
	mysql_free_result(res);
}

class MySQLResultReader {
public:
	virtual ~MySQLResultReader() = default;

	virtual string GetString(idx_t col) = 0;
	virtual int32_t GetInt32(idx_t col) = 0;
	virtual int64_t GetInt64(idx_t col) = 0;
	virtual bool IsNull(idx_t col) = 0;

	virtual DataChunk &NextChunk() = 0;
	virtual bool Next() = 0;
	virtual bool Exhausted() = 0;
	virtual const vector<MySQLField> &Fields() = 0;
};

class MySQLResult : public MySQLResultReader {
public:
	MySQLResult(const std::string &query_p, MySQLStatementPtr stmt_p, MySQLTypeConfig type_config_p,
	            const string &connection_string_p, unsigned long connection_id_p, MySQLResultStreaming streaming_p,
	            idx_t affected_rows_p, vector<MySQLField> fields_p = vector<MySQLField>());

	~MySQLResult() override;

	string GetString(idx_t col) override;
	int32_t GetInt32(idx_t col) override;
	int64_t GetInt64(idx_t col) override;
	bool IsNull(idx_t col) override;

	DataChunk &NextChunk() override;
	bool Next() override;
	bool Exhausted() override;
	idx_t AffectedRows();
	const vector<MySQLField> &Fields() override;

private:
	string query;
	MySQLStatementPtr stmt;
	MySQLTypeConfig type_config;
	string connection_string;
	unsigned long connection_id;
	MySQLResultStreaming streaming;
	idx_t affected_rows = static_cast<idx_t>(-1);

	vector<MySQLField> fields;

	DataChunk data_chunk;
	idx_t row_idx = static_cast<idx_t>(-1);
	bool exhausted = false;

	bool FetchNext();
	void HandleTruncatedData();
	void WriteToChunk(idx_t row);
	void CheckColumnIdx(idx_t col);
	void CheckNotNull(idx_t col);
	void CheckType(idx_t col, LogicalTypeId type_id);
	bool TryCancelQuery();
};

class MySQLTextResult : public MySQLResultReader {
public:
	MySQLTextResult(const std::string &query_p, MySQLResultPtr result_p, idx_t affected_rows_p);
	~MySQLTextResult() override = default;

	string GetString(idx_t col) override;
	int32_t GetInt32(idx_t col) override;
	int64_t GetInt64(idx_t col) override;
	bool IsNull(idx_t col) override;

	DataChunk &NextChunk() override;
	bool Next() override;
	bool Exhausted() override;
	const vector<MySQLField> &Fields() override;

private:
	void CheckColumnIdx(idx_t col);
	void CheckNotNull(idx_t col);

	string query;
	MySQLResultPtr result;
	idx_t affected_rows = static_cast<idx_t>(-1);
	vector<MySQLField> fields;
	DataChunk data_chunk;
	idx_t row_idx = static_cast<idx_t>(-1);
	bool exhausted = false;
};

} // namespace duckdb
