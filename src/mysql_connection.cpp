#include "mysql_connection.hpp"

#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/storage/table_storage_info.hpp"

#include "mysql_parameter.hpp"
#include "mysql_types.hpp"

namespace duckdb {

static bool debug_mysql_print_queries = false;

MySQLConnection::MySQLConnection(shared_ptr<OwnedMySQLConnection> connection_p, MySQLTypeConfig type_config_p,
                                 const string &connection_string_p)
    : connection(std::move(connection_p)), type_config(std::move(type_config_p)),
      connection_string(connection_string_p) {
}

MySQLConnection::~MySQLConnection() {
	Close();
}

MySQLConnection::MySQLConnection(MySQLConnection &&other) noexcept {
	std::swap(connection, other.connection);
	std::swap(type_config, other.type_config);
	std::swap(connection_string, other.connection_string);
}

MySQLConnection &MySQLConnection::operator=(MySQLConnection &&other) noexcept {
	std::swap(connection, other.connection);
	std::swap(type_config, other.type_config);
	std::swap(connection_string, other.connection_string);
	return *this;
}

MySQLConnection MySQLConnection::Open(MySQLTypeConfig type_config, const string &connection_string,
                                      const string &attach_path) {
	auto connection = make_shared_ptr<OwnedMySQLConnection>(MySQLUtils::Connect(connection_string, attach_path));
	return MySQLConnection(std::move(connection), std::move(type_config), connection_string);
}

idx_t MySQLConnection::MySQLExecute(MYSQL_STMT *stmt, const string &query, const vector<Value> &params, bool streaming,
                                    bool prepared) {
	if (MySQLConnection::DebugPrintQueries()) {
		Printer::Print(query + "\n");
	}

	lock_guard<mutex> l(query_lock);

	if (!stmt) { // basic interface
		auto con = GetConn();
		int res_query = mysql_real_query(con, query.c_str(), query.size());
		if (res_query != 0) {
			throw IOException("Failed to run query \"%s\": %s\n", query.c_str(), mysql_error(con));
		}
		auto result = MySQLResultPtr(mysql_store_result(con), MySQLResultDelete);
		return 0;
	}

	// statement interface, may or may not be already prepared

	if (!prepared) {
		int res_prepare = mysql_stmt_prepare(stmt, query.c_str(), query.size());
		if (res_prepare != 0) {
			throw IOException("Failed to prepare MySQL query \"%s\": %s\n", query.c_str(), mysql_stmt_error(stmt));
		}
	}

	vector<MySQLParameter> mysql_params;
	vector<MYSQL_BIND> binds;
	if (params.size() > 0) {
		size_t expected_count = mysql_stmt_param_count(stmt);
		if (expected_count != params.size()) {
			throw IOException(
			    "Incorrect query parameters count specified, expected: %zu, actual: %zu, MySQL query \"%s\": %s\n",
			    expected_count, params.size(), query.c_str(), mysql_stmt_error(stmt));
		}
		mysql_params.reserve(params.size());
		binds.reserve(params.size());
		for (const Value &dp : params) {
			mysql_params.emplace_back(query, dp);
			binds.push_back(mysql_params.back().CreateBind());
		}
		auto res_bind = mysql_stmt_bind_param(stmt, binds.data());
		if (res_bind != 0) {
			throw IOException("Failed to bind parameters, count: %zu, MySQL query \"%s\": %s\n", binds.size(),
			                  query.c_str(), mysql_stmt_error(stmt));
		}
	}

	int res_exec = mysql_stmt_execute(stmt);
	if (res_exec != 0) {
		throw IOException("Failed to execute MySQL query \"%s\": %s\n", query.c_str(), mysql_stmt_error(stmt));
	}

	idx_t affected_rows = mysql_stmt_affected_rows(stmt);

	if (!streaming && affected_rows == static_cast<idx_t>(-1)) {
		bool btrue = true;
		auto res_attr = mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &btrue);
		if (res_attr != 0) {
			throw IOException("Failed to set STMT_ATTR_UPDATE_MAX_LENGTH for MySQL query \"%s\": %s\n", query.c_str(),
			                  mysql_stmt_error(stmt));
		}

		int res_store = mysql_stmt_store_result(stmt);
		if (res_store != 0) {
			throw IOException("Failed to store result for MySQL query \"%s\": %s\n", query.c_str(),
			                  mysql_stmt_error(stmt));
		}
	}

	return affected_rows;
}

unique_ptr<MySQLResult> MySQLConnection::QueryInternal(const string &query, const vector<Value> &params,
                                                       MySQLResultStreaming streaming,
                                                       MySQLConnectorInterface con_interface) {
	auto con = GetConn();
	bool result_streaming = streaming == MySQLResultStreaming::ALLOW_STREAMING;
	bool basic_interface = con_interface == MySQLConnectorInterface::BASIC;

	if (basic_interface) {
		MySQLExecute(nullptr, query, params, result_streaming);
		return unique_ptr<MySQLResult>(nullptr);
	}

	auto stmt = MySQLStatementPtr(mysql_stmt_init(con), MySQLStatementDelete);
	if (!stmt) {
		throw IOException("Failed to initialize MySQL query \"%s\": %s\n", query.c_str(), mysql_error(con));
	}
	idx_t affected_rows = MySQLExecute(stmt.get(), query, params, result_streaming);
	unsigned long connection_id = connection->GetID();
	return make_uniq<MySQLResult>(query, std::move(stmt), type_config, connection_string, connection_id, streaming,
	                              affected_rows);
}

unique_ptr<MySQLResult> MySQLConnection::Query(const string &query, MySQLResultStreaming streaming) {
	return QueryInternal(query, vector<Value>(), streaming, MySQLConnectorInterface::PREPARED_STATEMENT);
}

unique_ptr<MySQLResult> MySQLConnection::Query(const string &query, const vector<Value> &params,
                                               MySQLResultStreaming streaming) {
	return QueryInternal(query, params, streaming, MySQLConnectorInterface::PREPARED_STATEMENT);
}

unique_ptr<MySQLTextResult> MySQLConnection::QueryText(const string &query) {
	if (MySQLConnection::DebugPrintQueries()) {
		Printer::Print(query + "\n");
	}

	lock_guard<mutex> l(query_lock);
	auto con = GetConn();
	int res_query = mysql_real_query(con, query.c_str(), query.size());
	if (res_query != 0) {
		throw IOException("Failed to run text protocol MySQL query \"%s\": %s\n", query.c_str(), mysql_error(con));
	}

	auto result = MySQLResultPtr(mysql_store_result(con), MySQLResultDelete);
	auto affected_rows = mysql_affected_rows(con);
	return make_uniq<MySQLTextResult>(query, std::move(result), affected_rows);
}

unique_ptr<MySQLResult> MySQLConnection::Query(MySQLStatement &stmt, const vector<Value> &params,
                                               MySQLResultStreaming streaming) {

	bool result_streaming = streaming == MySQLResultStreaming::ALLOW_STREAMING;
	bool prepared = true;
	idx_t affected_rows = MySQLExecute(stmt.get(), stmt.Query(), params, result_streaming, prepared);
	auto stmt_ptr = stmt.release();
	unsigned long connection_id = connection->GetID();
	return make_uniq<MySQLResult>(stmt.Query(), std::move(stmt_ptr), type_config, connection_string, connection_id,
	                              streaming, affected_rows, stmt.FieldsCopy());
}

unique_ptr<MySQLStatement> MySQLConnection::Prepare(const string &query) {
	auto con = GetConn();

	auto stmt = MySQLStatementPtr(mysql_stmt_init(con), MySQLStatementDelete);
	if (!stmt) {
		throw IOException("Failed to initialize MySQL query \"%s\": %s\n", query.c_str(), mysql_error(con));
	}

	int res_prepare = mysql_stmt_prepare(stmt.get(), query.c_str(), query.size());
	if (res_prepare != 0) {
		throw IOException("Failed to prepare MySQL query \"%s\": %s\n", query.c_str(), mysql_stmt_error(stmt.get()));
	}

	vector<MySQLField> fields = MySQLField::ReadFields(query, stmt.get(), type_config);
	if (fields.empty()) {
		throw InvalidInputException("Failed to fetch return types for query '%s'", query);
	}

	return make_uniq<MySQLStatement>(query, stmt.release(), std::move(fields));
}

static bool IsStarRocksVersion(const string &input) {
	return StringUtil::Contains(StringUtil::Lower(input), "starrocks");
}

static int ParseMajorVersion(const string &version) {
	auto dot = version.find('.');
	auto major_text = dot == string::npos ? version : version.substr(0, dot);
	try {
		return std::stoi(major_text);
	} catch (...) {
		return -1;
	}
}

MySQLBackendCapabilities MySQLConnection::DetectBackendCapabilities() {
	MySQLBackendCapabilities caps;
	auto con = GetConn();
	MySQLConnectionParameters connection_params;
	unordered_set<string> unused;
	std::tie(connection_params, unused) = MySQLUtils::ParseConnectionParameters(connection_string);
	auto server_info = mysql_get_server_info(con);
	if (server_info) {
		caps.version = server_info;
	}

	auto read_scalar_text = [&](const string &sql) -> string {
		int res_query = mysql_real_query(con, sql.c_str(), sql.size());
		if (res_query != 0) {
			return string();
		}
		auto result = MySQLResultPtr(mysql_store_result(con), MySQLResultDelete);
		if (!result) {
			return string();
		}
		auto row = mysql_fetch_row(result.get());
		if (!row || !row[0]) {
			return string();
		}
		return row[0];
	};
	auto text_query_succeeds = [&](const string &sql) -> bool {
		int res_query = mysql_real_query(con, sql.c_str(), sql.size());
		if (res_query != 0) {
			return false;
		}
		auto result = MySQLResultPtr(mysql_store_result(con), MySQLResultDelete);
		return true;
	};

	string version = read_scalar_text("SELECT VERSION()");
	if (!version.empty()) {
		caps.version = version;
	}
	caps.version_comment = read_scalar_text("SELECT @@version_comment");
	string current_version = read_scalar_text("SELECT current_version()");
	bool starrocks_show_frontends = text_query_succeeds("SHOW FRONTENDS");
	bool starrocks_port_hint = connection_params.port == 9030 && StringUtil::StartsWith(caps.version, "5.1");

	bool starrocks_version_text = IsStarRocksVersion(caps.version) || IsStarRocksVersion(caps.version_comment) ||
	                              IsStarRocksVersion(current_version);
	if (starrocks_version_text || starrocks_show_frontends || starrocks_port_hint) {
		auto starrocks_major = !current_version.empty() ? ParseMajorVersion(current_version) : ParseMajorVersion(caps.version);
		if (starrocks_port_hint || starrocks_show_frontends || (starrocks_major >= 0 && starrocks_major < 3)) {
			caps.kind = MySQLBackendKind::STARROCKS_LEGACY;
			caps.supports_prepared_statement = false;
			caps.supports_information_schema_schemata = false;
			return caps;
		}
		caps.kind = MySQLBackendKind::MYSQL_LIKE_UNKNOWN;
	}

	if (StringUtil::Contains(StringUtil::Lower(caps.version), "mariadb") ||
	    StringUtil::Contains(StringUtil::Lower(caps.version_comment), "mariadb")) {
		caps.kind = MySQLBackendKind::MARIADB_STANDARD;
	} else if (caps.kind == MySQLBackendKind::MYSQL_LIKE_UNKNOWN) {
		caps.kind = MySQLBackendKind::MYSQL_STANDARD;
	}
	return caps;
}

void MySQLConnection::Execute(const string &query) {
	Execute(query, vector<Value>());
}

void MySQLConnection::Execute(const string &query, const vector<Value> &params) {
	MySQLConnectorInterface con_interface =
	    params.size() > 0 ? MySQLConnectorInterface::PREPARED_STATEMENT : MySQLConnectorInterface::BASIC;
	QueryInternal(query, params, MySQLResultStreaming::FORCE_MATERIALIZATION, con_interface);
}

bool MySQLConnection::IsOpen() {
	return connection.get();
}

void MySQLConnection::Close() {
	if (!IsOpen()) {
		return;
	}
	connection = nullptr;
}

vector<IndexInfo> MySQLConnection::GetIndexInfo(const string &table_name) {
	return vector<IndexInfo>();
}

void MySQLConnection::DebugSetPrintQueries(bool print) {
	debug_mysql_print_queries = print;
}

bool MySQLConnection::DebugPrintQueries() {
	return debug_mysql_print_queries;
}

} // namespace duckdb
