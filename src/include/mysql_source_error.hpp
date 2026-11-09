#pragma once
#include "guarded_source_error.hpp"
#include "query_source_error.hpp"
#include "mysql_utils.hpp"
namespace duckdb {
inline string MySQLQueryErrorMessage(MYSQL *connection, MYSQL_STMT *statement, const string &stage,
                                     const string &query = "") {
	GuardedSourceError e;
	e.product = "mysql";
	auto state = statement ? mysql_stmt_sqlstate(statement) : mysql_sqlstate(connection);
	auto message = statement ? mysql_stmt_error(statement) : mysql_error(connection);
	e.state = state ? string(state) : string();
	e.code = std::to_string(statement ? mysql_stmt_errno(statement) : mysql_errno(connection));
	e.message = message ? string(message) : string();
	if (e.message.empty())
		e.message = "MySQL operation failed without a source diagnostic";
	return QuerySourceMessage(e.state.empty() || e.state == "00000" ? "" : e.JSON(), e.message, stage, query);
}
} // namespace duckdb
