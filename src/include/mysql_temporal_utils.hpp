#pragma once

#include <cctype>
#include <cstdint>
#include <string>

namespace duckdb {

inline bool IsAllowedMySQLSessionTimeZone(const std::string &tz) {
	size_t start = 0;
	while (start < tz.size() && (tz[start] == ' ' || tz[start] == '\t' || tz[start] == '\r' || tz[start] == '\n')) {
		start++;
	}
	size_t end = tz.size();
	while (end > start &&
	       (tz[end - 1] == ' ' || tz[end - 1] == '\t' || tz[end - 1] == '\r' || tz[end - 1] == '\n')) {
		end--;
	}
	std::string trimmed = tz.substr(start, end - start);
	if (trimmed.empty() || trimmed == "+00:00" || trimmed == "-00:00" || trimmed == "00:00") {
		return true;
	}
	std::string lower;
	lower.reserve(trimmed.size());
	for (unsigned char c : trimmed) {
		lower.push_back(static_cast<char>(std::tolower(c)));
	}
	return lower == "utc" || lower == "etc/utc" || lower == "gmt" || lower == "etc/gmt" || lower == "z";
}

inline bool IsMySQLDateYearInRange(int32_t year) {
	return year >= 1 && year <= 9999;
}

static constexpr int64_t MYSQL_TIMESTAMP_MIN_MICROS = 1000000LL;
static constexpr int64_t MYSQL_TIMESTAMP_MAX_MICROS = 2147483647999999LL;

inline bool IsMySQLTimestampUTCInRange(int64_t micros_since_epoch) {
	return micros_since_epoch >= MYSQL_TIMESTAMP_MIN_MICROS && micros_since_epoch <= MYSQL_TIMESTAMP_MAX_MICROS;
}

} // namespace duckdb
