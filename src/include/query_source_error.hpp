// Shared transport for source diagnostics across independently built scanners.
// Keep scanner copies identical using scripts/sync_query_source_error.py.
#pragma once
#include "duckdb.hpp"

namespace duckdb {
inline string QuerySourceQuote(const string &value) {
	const char *hex = "0123456789abcdef";
	string out = "\"";
	for (unsigned char ch : value) {
		if (ch == '"' || ch == '\\') {
			out += '\\';
			out += ch;
		} else if (ch < 32) {
			out += "\\u00";
			out += hex[ch >> 4];
			out += hex[ch & 15];
		} else {
			out += ch;
		}
	}
	return out + "\"";
}

inline string QuerySourceMessage(const string &document, const string &message, const string &stage,
                                 const string &query = "") {
	if (document.empty()) {
		return "Source operation failed at " + stage + ": " + message;
	}
	// Rooted, one-line JSON, never an SQLSTATE guessed from free-form prose.
	// A readable message follows for standalone DuckDB users. The Go boundary
	// only decodes the first line of native IO/Binder errors, never quoted SQL.
	auto envelope = "{\"source_error\":" + document + ",\"stage\":" + QuerySourceQuote(stage) +
	                ",\"query\":" + QuerySourceQuote(query) + "}";
	if (envelope.size() > 1024 * 1024) {
		return "Source operation failed at " + stage + ": " + message;
	}
	return "FQE_SOURCE_ERROR " + envelope + "\n" + message;
}
} // namespace duckdb
