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
	if (statement && connection && (e.state.empty() || e.state == "00000")) {
		auto connection_state = mysql_sqlstate(connection);
		if (connection_state && string(connection_state) != "00000" && mysql_errno(connection)) {
			e.state = connection_state;
			e.code = std::to_string(mysql_errno(connection));
			e.message = mysql_error(connection);
		}
	}
	if (e.message.empty()) {
		e.message = "MySQL operation failed without a source diagnostic";
	}
	if (e.state.empty() || e.state == "00000") {
		// Preserve evidence even when the client library supplies no SQLSTATE.
		// Do not manufacture a source state from the message or a fetch code.
		e.message += "; query_context={\"stage\":" + QuerySourceQuote(stage) +
		             ",\"query\":" + QuerySourceQuote(query) + ",\"protocol\":" +
		             QuerySourceQuote(statement ? "prepared" : "text") +
		             ",\"statement_errno\":" + std::to_string(statement ? mysql_stmt_errno(statement) : 0) +
		             ",\"connection_errno\":" + std::to_string(connection ? mysql_errno(connection) : 0) +
		             ",\"statement_state\":" + QuerySourceQuote(statement ? mysql_stmt_sqlstate(statement) : "unavailable") +
		             ",\"statement_message\":" + QuerySourceQuote(statement ? mysql_stmt_error(statement) : "unavailable") +
		             ",\"connection_state\":" + QuerySourceQuote(connection ? mysql_sqlstate(connection) : "unavailable") +
		             ",\"connection_message\":" + QuerySourceQuote(connection ? mysql_error(connection) : "unavailable") + "}";
	}
	return QuerySourceMessage(e.state.empty() || e.state == "00000" ? "" : e.JSON(), e.message, stage, query);
}
} // namespace duckdb
