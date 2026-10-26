#pragma once
#include "duckdb.hpp"
#include "duckdb/parser/keyword_helper.hpp"

// Typed v2 execution contract. No input contains native SQL. The source DDL is
// generated independently in each scanner after validating every represented
// field. This initial native surface intentionally excludes conditional DDL,
// ONLY/inheritance, implicit column attributes and non-equivalent conversions.
namespace duckdb {
struct GuardedDDLIntent {
	string database, schema, operation, action, name, index, owner, sql;
	vector<string> columns, keys;
	static bool Name(const string &s) { return !s.empty() && s.size() <= 63 && s.find('\0') == string::npos; }
	static string Text(const Value &v) {
		if (v.IsNull() || v.type().id() != LogicalTypeId::VARCHAR)
			throw BinderException("DDL structured input is invalid");
		return v.GetValue<string>();
	}
	static GuardedDDLIntent Bind(const vector<Value> &input, bool mysql) {
		if (input.size() != 5)
			throw BinderException("DDL structured input is invalid");
		GuardedDDLIntent d;
		d.database = Text(input[1]);
		d.schema = Text(input[2]);
		d.operation = Text(input[3]);
		if (!Name(d.database) || !Name(d.schema) || d.operation.size() != 36)
			throw BinderException("DDL source scope is invalid");
		for (idx_t i = 0; i < d.operation.size(); i++) {
			auto c = d.operation[i];
			if ((i == 8 || i == 13 || i == 18 || i == 23) ? c != '-'
														  : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
				throw BinderException("DDL operation identity is invalid");
		}
		if (mysql && d.database != d.schema)
			throw BinderException("DDL source scope is invalid");
		const auto &v = input[4];
		if (v.IsNull() || v.type().id() != LogicalTypeId::STRUCT)
			throw BinderException("DDL structured input is invalid");
		const auto &types = StructType::GetChildTypes(v.type());
		const auto &fields = StructValue::GetChildren(v);
		const vector<string> names = {"contract_version", "target_contract", "action",	"name",
									  "index_name",		  "owner",			 "columns", "keys"};
		if (types.size() != names.size())
			throw BinderException("DDL fields are invalid");
		for (idx_t i = 0; i < names.size(); i++)
			if (types[i].first != names[i] || fields[i].IsNull())
				throw BinderException("DDL fields are invalid");
		if (fields[0].type().id() != LogicalTypeId::INTEGER || fields[0].GetValue<int32_t>() != 2 ||
			Text(fields[1]) != "EXECUTION_NAME")
			throw BinderException("DDL contract is unavailable");
		d.action = Text(fields[2]);
		d.name = Text(fields[3]);
		d.index = Text(fields[4]);
		d.owner = Text(fields[5]);
		if (!Name(d.name) || (!d.index.empty() && !Name(d.index)) || (!d.owner.empty() && !Name(d.owner)))
			throw BinderException("DDL target name is invalid");
		if (fields[6].type().id() != LogicalTypeId::LIST || fields[7].type().id() != LogicalTypeId::LIST)
			throw BinderException("DDL payload is invalid");
		const auto &cols = ListValue::GetChildren(fields[6]);
		const auto &keys = ListValue::GetChildren(fields[7]);
		if (cols.size() > 256 || keys.size() > 32)
			throw BinderException("DDL payload bound exceeded");
		auto quote = [&](const string &s) { return KeywordHelper::WriteQuoted(s, mysql ? '`' : '"'); };
		vector<string> seen;
		for (auto &col : cols) {
			if (col.IsNull() || col.type().id() != LogicalTypeId::STRUCT)
				throw BinderException("DDL column definition is invalid");
			const auto &ct = StructType::GetChildTypes(col.type());
			const auto &cv = StructValue::GetChildren(col);
			if (ct.size() != 3 || ct[0].first != "name" || ct[1].first != "type" || ct[2].first != "length" ||
				cv[2].IsNull() || cv[2].type().id() != LogicalTypeId::INTEGER)
				throw BinderException("DDL column fields are invalid");
			string n = Text(cv[0]), type = Text(cv[1]);
			if (!Name(n) || std::find(seen.begin(), seen.end(), n) != seen.end())
				throw BinderException("DDL duplicate column is invalid");
			seen.push_back(n);
			auto length = cv[2].GetValue<int32_t>();
			string native;
			if (type == "smallint" || type == "integer" || type == "bigint") {
				if (length)
					throw BinderException("DDL column length is invalid");
				native = type;
			} else if (type == "varchar" && length >= 1 && length <= (mysql ? 16383 : 10485760)) {
				native = "VARCHAR(" + std::to_string(length) + ")";
			} else if (type == "text" && !mysql && !length) {
				native = "TEXT";
			} else {
				throw BinderException("DDL column type is unsupported");
			}
			d.columns.push_back(quote(n) + " " + native);
		}
		for (auto &key : keys) {
			string n = Text(key);
			if (!Name(n) || std::find(d.keys.begin(), d.keys.end(), n) != d.keys.end())
				throw BinderException("DDL index key is invalid");
			d.keys.push_back(n);
		}
		const auto target = quote(d.schema) + "." + quote(d.name);
		if (d.action == "create_table") {
			if (d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty())
				throw BinderException("DDL create table options are invalid");
			d.sql = "CREATE TABLE " + target + " (" + StringUtil::Join(d.columns, ",") + ")";
			if (mysql)
				d.sql += " ENGINE=InnoDB";
		} else if (d.action == "drop_table") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty())
				throw BinderException("DDL drop table options are invalid");
			d.sql = "DROP TABLE " + target + (mysql ? "" : " RESTRICT");
		} else if (d.action == "create_index" || d.action == "create_unique_index") {
			if (!d.columns.empty() || d.keys.empty() || d.index.empty() || !d.owner.empty())
				throw BinderException("DDL create index options are invalid");
			vector<string> keySQL;
			for (auto &key : d.keys)
				keySQL.push_back(quote(key));
			d.sql = string(d.action == "create_unique_index" ? "CREATE UNIQUE INDEX " : "CREATE INDEX ") +
					quote(d.index) + (mysql ? " USING BTREE ON " : " ON ") + target +
					(mysql ? " (" : " USING btree (") + StringUtil::Join(keySQL, ",") + ")";
		} else if (d.action == "drop_index") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() ||
				(mysql ? d.owner.empty() : !d.owner.empty()))
				throw BinderException("DDL drop index options are invalid");
			d.sql = "DROP INDEX " +
					(mysql ? quote(d.name) + " ON " + quote(d.schema) + "." + quote(d.owner) : target + " RESTRICT");
		} else {
			throw BinderException("DDL action is unsupported");
		}
		if (d.sql.size() > 65536)
			throw BinderException("DDL generated statement bound exceeded");
		return d;
	}
};
} // namespace duckdb
