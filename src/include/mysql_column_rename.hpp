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
	static std::string UpperWord(const std::string &s, size_t p, size_t len) {
		std::string out;
		out.reserve(len);
		for (size_t i = 0; i < len && p + i < s.size(); i++) {
			char c = s[p + i];
			if (c >= 'a' && c <= 'z') {
				c = static_cast<char>(c - 'a' + 'A');
			}
			out += c;
		}
		return out;
	}
	static bool IdentChar(char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
	}
	// Scan past `<type>[(args)] [UNSIGNED|SIGNED|ZEROFILL] [CHARACTER SET x] [CHARSET x] [COLLATE y]`.
	static size_t TypePrefixEnd(const std::string &s) {
		size_t p = 0;
		SkipSpace(s, p);
		size_t word_start = p;
		while (p < s.size() && IdentChar(s[p])) {
			p++;
		}
		if (p == word_start) {
			Invalid();
		}
		SkipSpace(s, p);
		// Handle multi-word base types like DOUBLE PRECISION
		if (p < s.size() && IdentChar(s[p])) {
			size_t save = p;
			size_t second_start = p;
			while (p < s.size() && IdentChar(s[p])) {
				p++;
			}
			auto second = UpperWord(s, second_start, p - second_start);
			if (second == "PRECISION" || second == "VARYING") {
				SkipSpace(s, p);
			} else {
				p = save;
			}
		}
		// Optional parenthesized modifiers e.g. (11), (18,4), ('a','b')
		if (p < s.size() && s[p] == '(') {
			int depth = 1;
			char q = 0;
			p++;
			while (p < s.size() && depth > 0) {
				char c = s[p++];
				if (q) {
					if (c == '\\' && q != '`') {
						if (p < s.size()) {
							p++;
						}
					} else if (c == q) {
						if (p < s.size() && s[p] == q) {
							p++;
						} else {
							q = 0;
						}
					}
				} else if (c == '\'' || c == '"' || c == '`') {
					q = c;
				} else if (c == '(') {
					depth++;
				} else if (c == ')') {
					depth--;
				}
			}
			if (depth != 0) {
				Invalid();
			}
			SkipSpace(s, p);
		}
		while (p < s.size()) {
			size_t save = p;
			if (!IdentChar(s[p])) {
				break;
			}
			size_t wstart = p;
			while (p < s.size() && IdentChar(s[p])) {
				p++;
			}
			auto kw = UpperWord(s, wstart, p - wstart);
			SkipSpace(s, p);
			if (kw == "UNSIGNED" || kw == "SIGNED" || kw == "ZEROFILL") {
				continue;
			}
			if (kw == "CHARACTER") {
				size_t set_start = p;
				while (p < s.size() && IdentChar(s[p])) {
					p++;
				}
				if (UpperWord(s, set_start, p - set_start) == "SET") {
					SkipSpace(s, p);
					size_t cs_start = p;
					while (p < s.size() && IdentChar(s[p])) {
						p++;
					}
					if (p > cs_start) {
						SkipSpace(s, p);
						continue;
					}
				}
				p = save;
				break;
			}
			if (kw == "CHARSET" || kw == "COLLATE") {
				size_t id_start = p;
				while (p < s.size() && IdentChar(s[p])) {
					p++;
				}
				if (p > id_start) {
					SkipSpace(s, p);
					continue;
				}
				p = save;
				break;
			}
			p = save;
			break;
		}
		return p;
	}
	// Replace only the leading type/charset/collation prefix so column nullability, defaults,
	// AUTO_INCREMENT, and comments are preserved.
	std::string AlterType(const std::string &schema, const std::string &table, const std::string &col_name,
	                      const std::string &new_type) const {
		for (auto &column : columns) {
			if (column.name != col_name) {
				continue;
			}
			const auto &s = column.definition;
			size_t p = TypePrefixEnd(s);
			std::string suffix = p < s.size() ? " " + s.substr(p) : std::string();
			return "ALTER TABLE " + Quote(schema) + "." + Quote(table) + " MODIFY COLUMN " + Quote(col_name) + " " +
			       new_type + suffix;
		}
		Invalid();
		return {};
	}
	// Rewrite only the top-level NOT NULL / NULL clause (and DEFAULT NULL when setting NOT NULL),
	// preserving type, charset/collation, defaults, AUTO_INCREMENT, and comments.
	std::string AlterNullability(const std::string &schema, const std::string &table, const std::string &col_name,
	                             bool set_not_null) const {
		for (auto &column : columns) {
			if (column.name != col_name) {
				continue;
			}
			const auto &s = column.definition;
			size_t p = TypePrefixEnd(s);
			size_t prefix_end = p;
			while (prefix_end > 0 && Space(s[prefix_end - 1])) {
				prefix_end--;
			}
			std::string prefix = s.substr(0, prefix_end);
			std::string tail = s.substr(p);
			std::string cleaned;
			char quote = 0;
			int depth = 0;
			bool comment = false;
			for (size_t i = 0; i < tail.size();) {
				char c = tail[i];
				if (comment) {
					cleaned += c;
					if (c == '*' && i + 1 < tail.size() && tail[i + 1] == '/') {
						cleaned += tail[i + 1];
						i += 2;
						comment = false;
						continue;
					}
					i++;
					continue;
				}
				if (quote) {
					cleaned += c;
					if (c == '\\' && quote != '`') {
						if (i + 1 < tail.size()) {
							cleaned += tail[++i];
						}
					} else if (c == quote) {
						if (i + 1 < tail.size() && tail[i + 1] == quote) {
							cleaned += tail[++i];
						} else {
							quote = 0;
						}
					}
					i++;
					continue;
				}
				if (c == '/' && i + 1 < tail.size() && tail[i + 1] == '*') {
					comment = true;
					cleaned += "/*";
					i += 2;
					continue;
				}
				if (c == '\'' || c == '"' || c == '`') {
					quote = c;
					cleaned += c;
					i++;
					continue;
				}
				if (c == '(') {
					depth++;
					cleaned += c;
					i++;
					continue;
				}
				if (c == ')') {
					if (depth > 0) {
						depth--;
					}
					cleaned += c;
					i++;
					continue;
				}
				if (depth == 0 && IdentChar(c) && (i == 0 || !IdentChar(tail[i - 1]))) {
					size_t wstart = i;
					while (i < tail.size() && IdentChar(tail[i])) {
						i++;
					}
					std::string kw = UpperWord(tail, wstart, i - wstart);
					if (kw == "NOT") {
						size_t after_sp = i;
						SkipSpace(tail, after_sp);
						size_t nw = after_sp;
						while (nw < tail.size() && IdentChar(tail[nw])) {
							nw++;
						}
						if (UpperWord(tail, after_sp, nw - after_sp) == "NULL") {
							i = nw;
							SkipSpace(tail, i);
							continue;
						}
					} else if (kw == "NULL") {
						SkipSpace(tail, i);
						continue;
					} else if (kw == "DEFAULT") {
						size_t after_sp = i;
						SkipSpace(tail, after_sp);
						size_t nw = after_sp;
						while (nw < tail.size() && IdentChar(tail[nw])) {
							nw++;
						}
						if (UpperWord(tail, after_sp, nw - after_sp) == "NULL") {
							if (set_not_null) {
								i = nw;
								SkipSpace(tail, i);
								continue;
							}
							cleaned += tail.substr(wstart, nw - wstart);
							i = nw;
							continue;
						}
					}
					cleaned += tail.substr(wstart, i - wstart);
					continue;
				}
				if (depth == 0 && Space(c)) {
					if (!cleaned.empty() && !Space(cleaned.back())) {
						cleaned += ' ';
					}
					i++;
					continue;
				}
				cleaned += c;
				i++;
			}
			size_t first = 0, last = cleaned.size();
			while (first < last && Space(cleaned[first])) {
				first++;
			}
			while (last > first && Space(cleaned[last - 1])) {
				last--;
			}
			cleaned = cleaned.substr(first, last - first);
			return "ALTER TABLE " + Quote(schema) + "." + Quote(table) + " MODIFY COLUMN " + Quote(col_name) + " " +
			       prefix + (set_not_null ? " NOT NULL" : " NULL") + (cleaned.empty() ? "" : " " + cleaned);
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
