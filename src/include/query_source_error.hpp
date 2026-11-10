// Shared transport for source diagnostics across independently built scanners.
// Keep scanner copies identical using scripts/sync_query_source_error.py.
#pragma once
#include "duckdb.hpp"
#include "duckdb/common/error_data.hpp"

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

inline string QueryScannerMessage(const string &driver, const string &kind, const string &message, const string &stage,
                                  const string &query = "", const string &column = "", const string &target_type = "") {
	auto fields = "{\"driver\":" + QuerySourceQuote(driver) + ",\"kind\":" + QuerySourceQuote(kind) +
	              ",\"message\":" + QuerySourceQuote(message) + ",\"stage\":" + QuerySourceQuote(stage) +
	              ",\"column\":" + QuerySourceQuote(column) + ",\"target_type\":" + QuerySourceQuote(target_type);
	auto envelope = fields + ",\"query\":" + QuerySourceQuote(query) + "}";
	if (envelope.size() > 1024 * 1024) {
		envelope = fields + ",\"query\":\"\",\"query_omitted\":true}";
	}
	return "FQE_SCANNER_ERROR " + envelope + "\n" + message;
}

// Adapter decoding failures are not source errors. System/IO/memory failures
// must retain their original exception; an empty return means rethrow.
inline string QueryResultErrorMessage(const std::exception &error, const string &driver, const string &query,
                                      const string &column, const string &target_type) {
	ErrorData data(error);
	string kind;
	switch (data.Type()) {
	case ExceptionType::CONVERSION:
	case ExceptionType::BINDER:
		kind = "result_conversion";
		break;
	case ExceptionType::INVALID_INPUT:
		kind = "result_invalid_input";
		break;
	case ExceptionType::OUT_OF_RANGE:
		kind = "result_out_of_range";
		break;
	case ExceptionType::NOT_IMPLEMENTED:
		kind = "result_type_not_supported";
		break;
	default:
		return "";
	}
	return QueryScannerMessage(driver, kind, error.what(), "result_decode", query, column, target_type);
}

inline string QuerySourceMessage(const string &document, const string &message, const string &stage,
                                 const string &query = "", const string &driver = "") {
	if (document.empty()) {
		return QueryScannerMessage(driver, "source_diagnostic_unavailable",
		                           message.empty() ? "Source operation failed without a driver diagnostic" : message,
		                           stage, query);
	}
	// Rooted, one-line JSON, never an SQLSTATE guessed from free-form prose.
	// A readable message follows for standalone DuckDB users. The Go boundary
	// only decodes the first line of native IO/Binder errors, never quoted SQL.
	auto envelope = "{\"source_error\":" + document + ",\"stage\":" + QuerySourceQuote(stage) +
	                ",\"query\":" + QuerySourceQuote(query) + "}";
	if (envelope.size() > 1024 * 1024) {
		// A large SQL string must not discard an otherwise valid source error.
		envelope = "{\"source_error\":" + document + ",\"stage\":" + QuerySourceQuote(stage) +
		           ",\"query\":\"\",\"query_omitted\":true}";
		if (envelope.size() > 1024 * 1024) {
			return QueryScannerMessage(driver, "source_diagnostic_unavailable", message, stage, query);
		}
	}
	return "FQE_SOURCE_ERROR " + envelope + "\n" + message;
}
} // namespace duckdb
