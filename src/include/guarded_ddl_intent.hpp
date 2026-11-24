#pragma once
#include "duckdb.hpp"
#include "duckdb/parser/keyword_helper.hpp"

// Typed v2/v3 execution contracts. No input contains native SQL. The source DDL is
// generated independently in each scanner after validating every represented
// field. This initial native surface intentionally excludes conditional DDL,
// ONLY/inheritance, implicit column attributes and non-equivalent conversions.
namespace duckdb {
struct GuardedDDLIntent {
	int32_t contract_version = 2;
	bool conditional = false;
	bool has_check_constraint = false;
	string database, schema, operation, action, name, index, owner, new_name, column_name, query_sql, sql;
	vector<string> columns, constraints, keys;
	vector<pair<string, string>> column_specs;
	string default_kind, default_value, constraint_name, constraint_kind, check_column, check_operator, check_value,
	    reference_schema, reference_table, identity_mode;
	vector<string> constraint_keys, reference_keys;
	static GuardedDDLIntent BindV4(const vector<Value> &input, bool mysql, bool conditional = false);
	static bool SimpleIntLiteral(const string &s) {
		if (s.empty() || s.size() > 38)
			return false;
		idx_t i = s[0] == '-' ? 1 : 0;
		if (i == s.size())
			return false;
		for (; i < s.size(); i++) {
			if (s[i] < '0' || s[i] > '9')
				return false;
		}
		return true;
	}
	static string DefaultLiteralSQL(const string &kind, const string &value, bool mysql) {
		if (kind == "null" && value.empty())
			return "NULL";
		if (kind == "integer" && SimpleIntLiteral(value))
			return value;
		if (kind == "boolean" && (value == "true" || value == "false"))
			return mysql ? (value == "true" ? "1" : "0") : (value == "true" ? "TRUE" : "FALSE");
		if (kind == "string" && value.size() <= 4096 && value.find('\0') == string::npos)
			return "'" + StringUtil::Replace(value, "'", "''") + "'";
		throw BinderException("DDL default literal is invalid");
	}
	static string ColumnTypeV5(const string &type, int32_t length, int32_t precision, int32_t scale, bool mysql) {
		if (type == "numeric") {
			if (length || precision < 1 || precision > (mysql ? 65 : 38) || scale < 0 || scale > precision ||
			    (mysql && scale > 30))
				throw BinderException("DDL numeric precision/scale is unsupported");
			return "DECIMAL(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
		}
		if (type == "timestamp" || type == "time" || (!mysql && type == "timestamptz")) {
			if (length || precision < 0 || precision > 6 || scale)
				throw BinderException("DDL temporal precision is unsupported");
			if (!mysql && type == "timestamptz")
				return "TIMESTAMP(" + std::to_string(precision) + ") WITH TIME ZONE";
			return string(type == "time" ? "TIME(" : (mysql ? "DATETIME(" : "TIMESTAMP(")) +
			       std::to_string(precision) + ")" + (mysql ? "" : " WITHOUT TIME ZONE");
		}
		if (precision || scale)
			throw BinderException("DDL unexpected type modifiers");
		if (type == "varchar" && length >= 1 && length <= (mysql ? 16383 : 10485760))
			return "VARCHAR(" + std::to_string(length) + ")";
		if (length)
			throw BinderException("DDL unexpected length");
		if (type == "smallint" || type == "integer" || type == "bigint" || type == "text" || type == "date")
			return type;
		if (type == "real")
			return mysql ? "FLOAT" : "REAL";
		if (type == "double")
			return mysql ? "DOUBLE" : "DOUBLE PRECISION";
		if (mysql) {
			if (type == "boolean")
				return "TINYINT(1)";
			if (type == "bytea")
				return "LONGBLOB";
			if (type == "uuid")
				return "CHAR(36)";
		} else if (type == "boolean" || type == "uuid" || type == "bytea") {
			return type;
		}
		throw BinderException("DDL column type is unsupported");
	}

	static bool Name(const string &s) {
		return !s.empty() && s.size() <= 63 && s.find('\0') == string::npos;
	}
	static string Text(const Value &v) {
		if (v.IsNull() || v.type().id() != LogicalTypeId::VARCHAR)
			throw BinderException("DDL structured input is invalid");
		return v.GetValue<string>();
	}
	static GuardedDDLIntent Bind(const vector<Value> &input, bool mysql, int32_t contract = 2,
	                             bool conditional = false) {
		if (contract == 4)
			return BindV4(input, mysql, conditional);
		if (input.size() != 5)
			throw BinderException("DDL structured input is invalid");
		GuardedDDLIntent d;
		d.contract_version = contract;
		d.conditional = conditional;
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
		const auto &v = input[4];
		if (v.IsNull() || v.type().id() != LogicalTypeId::STRUCT)
			throw BinderException("DDL structured input is invalid");
		const auto &types = StructType::GetChildTypes(v.type());
		const auto &fields = StructValue::GetChildren(v);
		vector<string> names = {"contract_version", "target_contract", "action", "name", "index_name", "owner"};
		if (contract == 3 || contract == 5 || contract == 6) {
			names.push_back("new_name");
			names.push_back("column_name");
		}
		if (contract == 6) {
			names.push_back("query_sql");
			names.push_back("constraints");
		}
		names.push_back("columns");
		names.push_back("keys");
		if (types.size() != names.size())
			throw BinderException("DDL fields are invalid");
		for (idx_t i = 0; i < names.size(); i++)
			if (types[i].first != names[i] || fields[i].IsNull())
				throw BinderException("DDL fields are invalid");
		if (fields[0].type().id() != LogicalTypeId::INTEGER || fields[0].GetValue<int32_t>() != contract ||
		    Text(fields[1]) != "EXECUTION_NAME")
			throw BinderException("DDL contract is unavailable");
		d.action = Text(fields[2]);
		if (mysql && d.database != d.schema && d.action != "create_schema" && d.action != "drop_schema")
			throw BinderException("DDL source scope is invalid");
		d.name = Text(fields[3]);
		d.index = Text(fields[4]);
		d.owner = Text(fields[5]);
		if (!Name(d.name) || (!d.index.empty() && !Name(d.index)) || (!d.owner.empty() && !Name(d.owner)))
			throw BinderException("DDL target name is invalid");
		const idx_t offset = (contract == 3 || contract == 5) ? 2 : (contract == 6 ? 4 : 0);
		if (contract == 3 || contract == 5 || contract == 6) {
			d.new_name = Text(fields[6]);
			d.column_name = Text(fields[7]);
			if ((!d.new_name.empty() && !Name(d.new_name)) || (!d.column_name.empty() && !Name(d.column_name)))
				throw BinderException("DDL altered name is invalid");
		}
		if (contract == 6) {
			d.query_sql = Text(fields[8]);
			if (d.query_sql.size() > 65536 || d.query_sql.find('\0') != string::npos)
				throw BinderException("DDL query payload is invalid");
		}
		const bool extended_action = d.action == "rename_table" || d.action == "add_column" ||
		                             d.action == "rename_column" || d.action == "rename_index";
		const bool v5_action = d.action == "create_table" || d.action == "add_column" || d.action == "drop_column" ||
		                       d.action == "alter_column_type" || d.action == "create_schema" ||
		                       d.action == "drop_schema";
		const bool v6_action = d.action == "create_table" || d.action == "add_column";
		if (contract == 6 ? !v6_action : contract == 5 ? !v5_action : extended_action != (contract == 3))
			throw BinderException("DDL action requires a different contract");
		if (fields[6 + offset].type().id() != LogicalTypeId::LIST ||
		    fields[7 + offset].type().id() != LogicalTypeId::LIST)
			throw BinderException("DDL payload is invalid");
		const auto &cols = ListValue::GetChildren(fields[6 + offset]);
		const auto &keys = ListValue::GetChildren(fields[7 + offset]);
		if (cols.size() > 256 || keys.size() > 32)
			throw BinderException("DDL payload bound exceeded");
		auto quote = [&](const string &s) {
			return KeywordHelper::WriteQuoted(s, mysql ? '`' : '"');
		};
		vector<string> seen;
		for (auto &col : cols) {
			if (col.IsNull() || col.type().id() != LogicalTypeId::STRUCT)
				throw BinderException("DDL column definition is invalid");
			const auto &ct = StructType::GetChildTypes(col.type());
			const auto &cv = StructValue::GetChildren(col);
			idx_t expected_fields = contract == 6 ? 10 : (contract == 5 ? 5 : 3);
			if (ct.size() != expected_fields || ct[0].first != "name" || ct[1].first != "type" ||
			    ct[2].first != "length" || cv[2].IsNull() || cv[2].type().id() != LogicalTypeId::INTEGER)
				throw BinderException("DDL column fields are invalid");
			string n = Text(cv[0]), type = Text(cv[1]);
			if (!Name(n) || std::find(seen.begin(), seen.end(), n) != seen.end())
				throw BinderException("DDL duplicate column is invalid");
			seen.push_back(n);
			auto length = cv[2].GetValue<int32_t>();
			string native, extra_col;
			if (contract == 5 || contract == 6) {
				if (ct[3].first != "precision" || ct[4].first != "scale" || cv[3].IsNull() || cv[4].IsNull() ||
				    cv[3].type().id() != LogicalTypeId::INTEGER || cv[4].type().id() != LogicalTypeId::INTEGER)
					throw BinderException("DDL numeric/time modifiers are invalid");
				native = ColumnTypeV5(type, length, cv[3].GetValue<int32_t>(), cv[4].GetValue<int32_t>(), mysql);
			} else if (type == "smallint" || type == "integer" || type == "bigint") {
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
			if (contract == 6) {
				if (ct[5].first != "not_null" || ct[6].first != "default_kind" || ct[7].first != "default_value" ||
				    ct[8].first != "primary_key" || ct[9].first != "unique" || cv[5].IsNull() || cv[8].IsNull() ||
				    cv[9].IsNull() || cv[5].type().id() != LogicalTypeId::BOOLEAN ||
				    cv[8].type().id() != LogicalTypeId::BOOLEAN || cv[9].type().id() != LogicalTypeId::BOOLEAN)
					throw BinderException("DDL v6 column modifiers are invalid");
				if (cv[5].GetValue<bool>())
					extra_col += " NOT NULL";
				string dk = Text(cv[6]), dv = Text(cv[7]);
				if (!dk.empty())
					extra_col += " DEFAULT " + DefaultLiteralSQL(dk, dv, mysql);
				else if (!dv.empty())
					throw BinderException("DDL column default kind is missing");
				if (cv[8].GetValue<bool>())
					extra_col += " PRIMARY KEY";
				if (cv[9].GetValue<bool>())
					extra_col += " UNIQUE";
			}
			d.columns.push_back(quote(n) + " " + native + extra_col);
			d.column_specs.emplace_back(n, native);
		}
		if (contract == 6) {
			if (fields[9].type().id() != LogicalTypeId::LIST)
				throw BinderException("DDL constraint list is invalid");
			const auto &cons = ListValue::GetChildren(fields[9]);
			if (cons.size() > 64)
				throw BinderException("DDL constraint bound exceeded");
			for (auto &c : cons) {
				if (c.IsNull() || c.type().id() != LogicalTypeId::STRUCT)
					throw BinderException("DDL constraint definition is invalid");
				const auto &ct = StructType::GetChildTypes(c.type());
				const auto &cv = StructValue::GetChildren(c);
				const vector<string> cnames = {"name",           "kind",          "columns",
				                               "reference_schema", "reference_table", "reference_columns",
				                               "check_column",   "check_operator", "check_value"};
				if (ct.size() != cnames.size())
					throw BinderException("DDL constraint fields are invalid");
				for (idx_t j = 0; j < cnames.size(); j++)
					if (ct[j].first != cnames[j] || cv[j].IsNull())
						throw BinderException("DDL constraint fields are invalid");
				string cname = Text(cv[0]), kind = Text(cv[1]);
				if (!cname.empty() && !Name(cname))
					throw BinderException("DDL constraint name is invalid");
				auto keys_sql = [&](const Value &lv) {
					if (lv.type().id() != LogicalTypeId::LIST)
						throw BinderException("DDL constraint keys are invalid");
					auto &items = ListValue::GetChildren(lv);
					if (items.empty() || items.size() > 32)
						throw BinderException("DDL constraint keys bound exceeded");
					vector<string> out;
					for (auto &item : items) {
						auto kn = Text(item);
						if (!Name(kn))
							throw BinderException("DDL constraint key name is invalid");
						out.push_back(quote(kn));
					}
					return "(" + StringUtil::Join(out, ",") + ")";
				};
				string def;
				if (kind == "primary_key") {
					def = "PRIMARY KEY " + keys_sql(cv[2]);
				} else if (kind == "unique") {
					def = "UNIQUE " + keys_sql(cv[2]);
				} else if (kind == "foreign_key") {
					string rs = Text(cv[3]), rt = Text(cv[4]);
					if (rs != d.schema || !Name(rt))
						throw BinderException("DDL foreign key target is invalid");
					auto ck = keys_sql(cv[2]), rk = keys_sql(cv[5]);
					if (ListValue::GetChildren(cv[2]).size() != ListValue::GetChildren(cv[5]).size())
						throw BinderException("DDL foreign key arity mismatch");
					def = "FOREIGN KEY " + ck + " REFERENCES " + quote(rs) + "." + quote(rt) + " " + rk +
					      " ON UPDATE NO ACTION ON DELETE NO ACTION";
				} else if (kind == "check") {
					string col = Text(cv[6]), op = Text(cv[7]), val = Text(cv[8]);
					if (!Name(col) || !SimpleIntLiteral(val) ||
					    (op != "=" && op != "<>" && op != "<" && op != ">" && op != "<=" && op != ">="))
						throw BinderException("DDL CHECK constraint is invalid");
					def = "CHECK (" + quote(col) + " " + op + " " + val + ")";
					d.has_check_constraint = true;
				} else {
					throw BinderException("DDL constraint kind is unsupported");
				}
				if (!cname.empty() && !(mysql && kind == "primary_key"))
					d.constraints.push_back("CONSTRAINT " + quote(cname) + " " + def);
				else
					d.constraints.push_back(def);
			}
		}
		for (auto &key : keys) {
			string n = Text(key);
			if (!Name(n) || std::find(d.keys.begin(), d.keys.end(), n) != d.keys.end())
				throw BinderException("DDL index key is invalid");
			d.keys.push_back(n);
		}
		const auto target = quote(d.schema) + "." + quote(d.name);
		if (d.action == "create_table") {
			if (d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() || !d.new_name.empty() ||
			    !d.column_name.empty())
				throw BinderException("DDL create table options are invalid");
			vector<string> defs = d.columns;
			defs.insert(defs.end(), d.constraints.begin(), d.constraints.end());
			d.sql = "CREATE TABLE " + string(conditional ? "IF NOT EXISTS " : "") + target + " (" +
			        StringUtil::Join(defs, ",") + ")";
			if (mysql)
				d.sql += " ENGINE=InnoDB";
		} else if (d.action == "drop_table") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty())
				throw BinderException("DDL drop table options are invalid");
			d.sql = "DROP TABLE " + string(conditional ? "IF EXISTS " : "") + target + (mysql ? "" : " RESTRICT");
		} else if (d.action == "create_index" || d.action == "create_unique_index") {
			if (!d.columns.empty() || d.keys.empty() || d.index.empty() || !d.owner.empty())
				throw BinderException("DDL create index options are invalid");
			vector<string> keySQL;
			for (auto &key : d.keys)
				keySQL.push_back(quote(key));
			d.sql = string(d.action == "create_unique_index" ? "CREATE UNIQUE INDEX " : "CREATE INDEX ") +
			        string(conditional && !mysql ? "IF NOT EXISTS " : "") + quote(d.index) +
			        (mysql ? " USING BTREE ON " : " ON ") + target + (mysql ? " (" : " USING btree (") +
			        StringUtil::Join(keySQL, ",") + ")";
		} else if (d.action == "drop_index") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() ||
			    (mysql ? (d.owner.empty() && !conditional) : !d.owner.empty()))
				throw BinderException("DDL drop index options are invalid");
			d.sql = "DROP INDEX " + (mysql ? quote(d.name) + " ON " + quote(d.schema) + "." + quote(d.owner)
			                               : string(conditional ? "IF EXISTS " : "") + target + " RESTRICT");
		} else if (d.action == "rename_table") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() ||
			    !d.column_name.empty() || d.new_name.empty() || d.new_name == d.name)
				throw BinderException("DDL rename table options are invalid");
			d.sql = mysql ? "RENAME TABLE " + target + " TO " + quote(d.schema) + "." + quote(d.new_name)
			              : "ALTER TABLE " + target + " RENAME TO " + quote(d.new_name);
		} else if (d.action == "add_column") {
			if (d.columns.size() != 1 || !d.keys.empty() || !d.index.empty() || !d.owner.empty() ||
			    !d.column_name.empty() || !d.new_name.empty() || !d.constraints.empty())
				throw BinderException("DDL add column options are invalid");
			// ONLY prevents implicit propagation into PG inheritance children.
			d.sql = "ALTER TABLE " + string(mysql ? "" : "ONLY ") + target + " ADD COLUMN " +
			        string(conditional && !mysql ? "IF NOT EXISTS " : "") + d.columns[0];
		} else if (d.action == "rename_column") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() ||
			    d.column_name.empty() || d.new_name.empty() || d.new_name == d.column_name)
				throw BinderException("DDL rename column options are invalid");
			d.sql = "ALTER TABLE " + string(mysql ? "" : "ONLY ") + target + " RENAME COLUMN " + quote(d.column_name) +
			        " TO " + quote(d.new_name);
		} else if (d.action == "rename_index") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.column_name.empty() ||
			    d.new_name.empty() || d.new_name == d.name || (mysql ? d.owner.empty() : !d.owner.empty()))
				throw BinderException("DDL rename index options are invalid");
			d.sql = mysql ? "ALTER TABLE " + quote(d.schema) + "." + quote(d.owner) + " RENAME INDEX " + quote(d.name) +
			                    " TO " + quote(d.new_name)
			              : "ALTER INDEX " + target + " RENAME TO " + quote(d.new_name);
		} else if (d.action == "drop_column") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() ||
			    !d.new_name.empty() || d.column_name.empty())
				throw BinderException("DDL drop column options are invalid");
			d.sql = "ALTER TABLE " + string(mysql ? "" : "ONLY ") + target + " DROP COLUMN " +
			        string(conditional && !mysql ? "IF EXISTS " : "") + quote(d.column_name) +
			        (mysql ? "" : " RESTRICT");
		} else if (d.action == "alter_column_type") {
			if (d.columns.size() != 1 || d.column_specs.size() != 1 || !d.keys.empty() || !d.index.empty() ||
			    !d.owner.empty() || !d.new_name.empty() || d.column_name.empty() ||
			    d.column_specs[0].first != d.column_name)
				throw BinderException("DDL alter column type options are invalid");
			d.sql = mysql
			            ? ("ALTER TABLE " + target + " MODIFY COLUMN " + d.columns[0])
			            : ("ALTER TABLE ONLY " + target + " ALTER COLUMN " + quote(d.column_name) + " TYPE " +
			               d.column_specs[0].second);
		} else if (d.action == "create_schema") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() ||
			    !d.new_name.empty() || !d.column_name.empty() || d.name != d.schema)
				throw BinderException("DDL create schema options are invalid");
			d.sql = "CREATE SCHEMA " + string(conditional ? "IF NOT EXISTS " : "") + quote(d.name);
		} else if (d.action == "drop_schema") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() ||
			    !d.new_name.empty() || !d.column_name.empty() || d.name != d.schema)
				throw BinderException("DDL drop schema options are invalid");
			d.sql = "DROP SCHEMA " + string(conditional ? "IF EXISTS " : "") + quote(d.name) + (mysql ? "" : " RESTRICT");
		} else {
			throw BinderException("DDL action is unsupported");
		}
		if (d.sql.size() > 65536)
			throw BinderException("DDL generated statement bound exceeded");
		return d;
	}
};
} // namespace duckdb

#include "guarded_ddl_v4.hpp"
