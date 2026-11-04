#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace duckdb {

// SHOW CREATE is read under the same source table lock and SQL mode used to
// execute CHANGE. Keep source column definitions verbatim: DuckDB's logical
// types and information_schema's default strings cannot reconstruct them.
struct MySQLRenameColumn {
	std::string name;
	std::string definition;
};

struct MySQLRenameDefinition {
	static constexpr size_t MAX_BYTES = 1024 * 1024;
	std::vector<MySQLRenameColumn> columns;

	static void Invalid() {
		throw std::invalid_argument("MySQL column definition is incomplete or ambiguous");
	}
	static bool Space(char c) {
		return c == ' ' || c == '\t' || c == '\n' || c == '\r';
	}
	static void SkipSpace(const std::string &s, size_t &p) {
		while (p < s.size() && Space(s[p])) {
			p++;
		}
	}
	static std::string Identifier(const std::string &s, size_t &p) {
		if (p >= s.size() || s[p++] != '`') {
			Invalid();
		}
		std::string out;
		while (p < s.size()) {
			auto c = s[p++];
			if (c == '`') {
				if (p < s.size() && s[p] == '`') {
					out += s[p++];
				} else {
					if (out.empty()) {
						Invalid();
					}
					return out;
				}
			} else {
				out += c;
			}
		}
		Invalid();
		return {};
	}
	static std::string Quote(const std::string &s) {
		if (s.empty() || s.size() > 63 || s.find('\0') != std::string::npos) {
			Invalid();
		}
		std::string out = "`";
		for (auto c : s) {
			out += c;
			if (c == '`') {
				out += c;
			}
		}
		return out + "`";
	}
	static MySQLRenameDefinition Parse(const std::string &s, const std::string &table) {
		if (s.empty() || s.size() > MAX_BYTES || s.find('\0') != std::string::npos ||
		    s.compare(0, 13, "CREATE TABLE ") != 0) {
			Invalid();
		}
		size_t p = 13;
		if (Identifier(s, p) != table) {
			Invalid();
		}
		SkipSpace(s, p);
		if (p == s.size() || s[p++] != '(') {
			Invalid();
		}
		MySQLRenameDefinition out;
		size_t start = p;
		int depth = 1;
		char quote = 0;
		bool comment = false;
		for (; p < s.size(); p++) {
			auto c = s[p];
			if (comment) {
				if (c == '*' && p + 1 < s.size() && s[p + 1] == '/') {
					comment = false;
					p++;
				}
				continue;
			}
			if (quote) {
				// MySQL 5.7 SHOW CREATE always backslash-escapes string contents,
				// even under NO_BACKSLASH_ESCAPES. The caller uses escape-enabled
				// mode for both SHOW CREATE and CHANGE. Identifiers double ` only.
				if (c == '\\' && quote != '`') {
					if (++p == s.size()) {
						Invalid();
					}
				} else if (c == quote) {
					if (p + 1 < s.size() && s[p + 1] == quote) {
						p++;
					} else {
						quote = 0;
					}
				}
				continue;
			}
			if (c == '`' || c == '\'' || c == '"') {
				quote = c;
			} else if (c == '/' && p + 1 < s.size() && s[p + 1] == '*') {
				comment = true;
				p++;
			} else if (c == ';' || c == '#' || (c == '-' && p + 1 < s.size() && s[p + 1] == '-')) {
				Invalid(); // SHOW CREATE does not need statement/line-comment delimiters here.
			} else if (c == '(') {
				if (++depth > 256) {
					Invalid();
				}
			} else if ((c == ',' && depth == 1) || (c == ')' && --depth == 0)) {
				size_t first = start, last = p;
				SkipSpace(s, first);
				while (last > first && Space(s[last - 1])) {
					last--;
				}
				if (first == last) {
					Invalid();
				}
				if (s[first] == '`') {
					auto name = Identifier(s, first);
					if (first >= last || !Space(s[first])) {
						Invalid();
					}
					SkipSpace(s, first);
					if (first >= last || out.columns.size() >= 4096) {
						Invalid();
					}
					for (auto &column : out.columns) {
						if (column.name == name) {
							Invalid();
						}
					}
					out.columns.push_back({name, s.substr(first, last - first)});
				}
				start = p + 1;
				if (!depth) {
					if (out.columns.empty()) {
						Invalid();
					}
					return out;
				}
			}
		}
		Invalid();
		return {};
	}
	std::string Change(const std::string &schema, const std::string &table, const std::string &old_name,
	                   const std::string &new_name) const {
		for (auto &column : columns) {
			if (column.name == old_name) {
				return "ALTER TABLE " + Quote(schema) + "." + Quote(table) + " CHANGE COLUMN " + Quote(old_name) +
				       " " + Quote(new_name) + " " + column.definition;
			}
		}
		Invalid();
		return {};
	}
	void StringDefault(const std::string &column_name, const std::string &charset, const std::string &hex) {
		if (charset.empty() || charset.size() > 64 || hex.size() > MAX_BYTES || hex.size() % 2) {
			Invalid();
		}
		for (auto c : charset) {
			if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_') {
				Invalid();
			}
		}
		for (auto c : hex) {
			if (!(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'F')) {
				Invalid();
			}
		}
		for (auto &column : columns) {
			if (column.name != column_name) {
				continue;
			}
			auto &s = column.definition;
			char quote = 0;
			int depth = 0;
			bool comment = false;
			for (size_t p = 0; p < s.size(); p++) {
				auto c = s[p];
				if (comment) {
					if (c == '*' && p + 1 < s.size() && s[p + 1] == '/') {
						comment = false;
						p++;
					}
				} else if (quote) {
					if (c == '\\' && quote != '`') {
						if (++p == s.size()) {
							Invalid();
						}
					} else if (c == quote) {
						if (p + 1 < s.size() && s[p + 1] == quote) {
							p++;
						} else {
							quote = 0;
						}
					}
				} else if (c == '/' && p + 1 < s.size() && s[p + 1] == '*') {
					comment = true;
					p++;
				} else if (c == '\'' || c == '"' || c == '`') {
					quote = c;
				} else if (c == '(') {
					depth++;
				} else if (c == ')') {
					depth--;
				} else if (!depth && (p == 0 || Space(s[p - 1])) && s.compare(p, 7, "DEFAULT") == 0 &&
				           p + 7 < s.size() && Space(s[p + 7])) {
					size_t first = p + 7;
					SkipSpace(s, first);
					if (first == s.size() || s[first] != '\'') {
						Invalid();
					}
					for (size_t last = first + 1; last < s.size(); last++) {
						if (s[last] == '\\') {
							last++;
						} else if (s[last] == '\'') {
							if (last + 1 < s.size() && s[last + 1] == '\'') {
								last++;
							} else {
								s.replace(first, last - first + 1, "_" + charset + " X'" + hex + "'");
								return;
							}
						}
					}
					Invalid();
				}
			}
		}
		Invalid();
	}
	// CHANGE may serialize an implicit nullable default as DEFAULT NULL. Only
	// remove this top-level clause, never words inside a type, literal, comment
	// or generated expression. The caller independently certifies nullable,
	// non-generated, NULL-default semantics from the locked source definition.
	static std::string WithoutNullDefault(const std::string &s) {
		char quote = 0;
		int depth = 0;
		bool comment = false;
		for (size_t p = 0; p < s.size(); p++) {
			auto c = s[p];
			if (comment) {
				if (c == '*' && p + 1 < s.size() && s[p + 1] == '/') {
					comment = false;
					p++;
				}
			} else if (quote) {
				if (c == '\\' && quote != '`') {
					if (++p == s.size()) return s;
				} else if (c == quote) {
					if (p + 1 < s.size() && s[p + 1] == quote) p++;
					else quote = 0;
				}
			} else if (c == '/' && p + 1 < s.size() && s[p + 1] == '*') {
				comment = true;
				p++;
			} else if (c == '\'' || c == '"' || c == '`') {
				quote = c;
			} else if (c == '(') {
				depth++;
			} else if (c == ')') {
				depth--;
			} else if (!depth && (p == 0 || Space(s[p - 1])) && s.compare(p, 7, "DEFAULT") == 0 &&
			           p + 7 < s.size() && Space(s[p + 7])) {
				size_t first = p + 7;
				SkipSpace(s, first);
				if (s.compare(first, 4, "NULL") != 0 || (first + 4 < s.size() && !Space(s[first + 4]))) return s;
				size_t start = p;
				while (start > 0 && Space(s[start - 1])) start--;
				size_t end = first + 4;
				if (end == s.size()) return s.substr(0, start);
				return s.substr(0, start) + s.substr(end);
			}
		}
		return s;
	}
	bool Preserved(const MySQLRenameDefinition &after, const std::string &old_name,
	               const std::string &new_name, bool nullable_null_default = false) const {
		if (columns.size() != after.columns.size()) {
			return false;
		}
		bool renamed = false;
		for (size_t i = 0; i < columns.size(); i++) {
			auto &before = columns[i];
			auto &next = after.columns[i];
			const bool target = before.name == old_name;
			if (next.name != (target ? new_name : before.name)) {
				return false;
			}
			if (next.definition != before.definition &&
			    !(target && nullable_null_default && WithoutNullDefault(next.definition) == WithoutNullDefault(before.definition))) {
				return false;
			}
			renamed = renamed || target;
		}
		return renamed;
	}
};
} // namespace duckdb
