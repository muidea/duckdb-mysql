#pragma once
#include <cerrno>
#include <cstdlib>

namespace duckdb {

inline string GuardedDDLIntent::ColumnTypeV6(const string &type, int32_t length, int32_t precision, int32_t scale,
                                            bool mysql) {
	if (type == "numeric") {
		if (length || precision < 1 || precision > (mysql ? 65 : 38) || scale < 0 || scale > precision ||
		    (mysql && scale > 30)) {
			throw BinderException("DDL numeric precision/scale is unsupported");
		}
		return "DECIMAL(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
	}
	if (type == "timestamp" || type == "timestamptz" || type == "time") {
		if (length || precision < 0 || precision > 6 || scale) {
			throw BinderException("DDL temporal precision is unsupported");
		}
		if (mysql) {
			if (type == "timestamp") {
				return "DATETIME(" + std::to_string(precision) + ")";
			}
			if (type == "timestamptz") {
				return "TIMESTAMP(" + std::to_string(precision) + ")";
			}
			return "TIME(" + std::to_string(precision) + ")";
		}
		if (type == "timestamptz") {
			return "TIMESTAMP(" + std::to_string(precision) + ") WITH TIME ZONE";
		}
		return string(type == "time" ? "TIME(" : "TIMESTAMP(") + std::to_string(precision) + ") WITHOUT TIME ZONE";
	}
	if (precision || scale) {
		throw BinderException("DDL unexpected type modifiers");
	}
	if (type == "varchar" && length >= 1 && length <= (mysql ? 16383 : 10485760)) {
		return "VARCHAR(" + std::to_string(length) + ")";
	}
	if (length) {
		throw BinderException("DDL unexpected length");
	}
	if (type == "smallint" || type == "integer" || type == "bigint" || type == "text" || type == "date") {
		return type;
	}
	if (type == "real") {
		return mysql ? "FLOAT" : "REAL";
	}
	if (type == "double") {
		return mysql ? "DOUBLE" : "DOUBLE PRECISION";
	}
	if (type == "boolean") {
		return mysql ? "BOOLEAN" : "boolean";
	}
	if (type == "bytea") {
		return mysql ? "LONGBLOB" : "bytea";
	}
	if (!mysql && type == "uuid") {
		return "uuid";
	}
	throw BinderException("DDL column type is unsupported");
}

inline GuardedDDLIntent GuardedDDLIntent::BindV6(const vector<Value> &input, bool mysql) {
	if (input.size() != 5) {
		throw BinderException("DDL structured input is invalid");
	}
	GuardedDDLIntent d;
	d.contract_version = 6;
	d.database = Text(input[1]);
	d.schema = Text(input[2]);
	d.operation = Text(input[3]);
	if (!Name(d.database) || !Name(d.schema) || (mysql && d.database != d.schema) || d.operation.size() != 36) {
		throw BinderException("DDL scope is invalid");
	}
	for (idx_t i = 0; i < d.operation.size(); i++) {
		auto c = d.operation[i];
		if ((i == 8 || i == 13 || i == 18 || i == 23) ? c != '-'
		                                              : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			throw BinderException("DDL operation identity is invalid");
		}
	}
	const auto &v = input[4];
	if (v.IsNull() || v.type().id() != LogicalTypeId::STRUCT) {
		throw BinderException("DDL input is invalid");
	}
	const auto &types = StructType::GetChildTypes(v.type());
	const auto &f = StructValue::GetChildren(v);
	const vector<string> names = {
	    "contract_version", "target_contract", "action",          "name",
	    "index_name",       "owner",           "new_name",        "column_name",
	    "default_kind",     "default_value",   "constraint_name", "constraint_kind",
	    "check_column",     "check_operator",  "check_value",     "reference_schema",
	    "reference_table",  "identity_mode",   "constraint_keys", "reference_keys",
	    "columns",          "keys",            "conditional",     "primary_key_name",
	    "primary_key_keys",
	};
	if (types.size() != names.size()) {
		throw BinderException("DDL fields are invalid");
	}
	for (idx_t i = 0; i < names.size(); i++) {
		if (types[i].first != names[i] || f[i].IsNull()) {
			throw BinderException("DDL fields are invalid");
		}
	}
	if (f[0].type().id() != LogicalTypeId::INTEGER || f[0].GetValue<int32_t>() != 6 || Text(f[1]) != "EXECUTION_NAME") {
		throw BinderException("DDL contract is unavailable");
	}
	if (f[22].type().id() != LogicalTypeId::BOOLEAN) {
		throw BinderException("DDL conditional flag is invalid");
	}
	d.action = Text(f[2]);
	d.name = Text(f[3]);
	d.index = Text(f[4]);
	d.owner = Text(f[5]);
	d.new_name = Text(f[6]);
	d.column_name = Text(f[7]);
	d.default_kind = Text(f[8]);
	d.default_value = Text(f[9]);
	d.constraint_name = Text(f[10]);
	d.constraint_kind = Text(f[11]);
	d.check_column = Text(f[12]);
	d.check_operator = Text(f[13]);
	d.check_value = Text(f[14]);
	d.reference_schema = Text(f[15]);
	d.reference_table = Text(f[16]);
	d.identity_mode = Text(f[17]);
	d.conditional = f[22].GetValue<bool>();
	d.primary_key_name = Text(f[23]);

	if (!Name(d.name)) {
		throw BinderException("DDL target is invalid");
	}
	for (const auto &n : {d.index, d.owner, d.new_name, d.column_name, d.constraint_name, d.check_column,
	                      d.reference_schema, d.reference_table, d.primary_key_name}) {
		if (!n.empty() && !Name(n)) {
			throw BinderException("DDL identifier is invalid");
		}
	}

	auto extract_key_list = [&](const Value &list_val, vector<string> &dest) {
		if (list_val.type().id() != LogicalTypeId::LIST) {
			throw BinderException("DDL list is invalid");
		}
		const auto &items = ListValue::GetChildren(list_val);
		if (items.size() > 32) {
			throw BinderException("DDL key limit exceeded");
		}
		for (const auto &item : items) {
			auto n = Text(item);
			if (!Name(n) || std::find(dest.begin(), dest.end(), n) != dest.end()) {
				throw BinderException("DDL key is invalid");
			}
			dest.push_back(n);
		}
	};
	extract_key_list(f[18], d.constraint_keys);
	extract_key_list(f[19], d.reference_keys);
	extract_key_list(f[21], d.keys);
	extract_key_list(f[24], d.primary_key_keys);

	if (f[20].type().id() != LogicalTypeId::LIST) {
		throw BinderException("DDL payload is invalid");
	}
	const auto &cols = ListValue::GetChildren(f[20]);
	if (cols.size() > 256) {
		throw BinderException("DDL payload bound exceeded");
	}

	auto integer = [](const string &s) {
		if (s.empty() || s.size() > 20 || s.find('\0') != string::npos) {
			return false;
		}
		errno = 0;
		char *end = nullptr;
		auto n = std::strtoll(s.c_str(), &end, 10);
		return errno != ERANGE && end == s.c_str() + s.size() && std::to_string(n) == s;
	};
	auto literal = [](const string &s) {
		return "'" + StringUtil::Replace(s, "'", "''") + "'";
	};
	auto quote = [&](const string &s) {
		return KeywordHelper::WriteQuoted(s, mysql ? '`' : '"');
	};
	auto default_literal = [&](const string &kind, const string &value) -> string {
		if (kind == "null" && value.empty()) {
			return "NULL";
		}
		if (kind == "integer" && integer(value)) {
			return value;
		}
		if (kind == "boolean" && (value == "true" || value == "false")) {
			return mysql ? (value == "true" ? "1" : "0") : (value == "true" ? "TRUE" : "FALSE");
		}
		if (kind == "string" && value.size() <= 4096 && value.find('\0') == string::npos) {
			return literal(value);
		}
		throw BinderException("DDL default literal is invalid");
	};
	auto keysSQL = [&](const vector<string> &keys) {
		vector<string> out;
		for (auto &k : keys) {
			out.push_back(quote(k));
		}
		return "(" + StringUtil::Join(out, ",") + ")";
	};

	vector<string> seen_cols;
	idx_t inline_pk_count = 0;
	string inline_pk_col;
	for (const auto &col : cols) {
		if (col.IsNull() || col.type().id() != LogicalTypeId::STRUCT) {
			throw BinderException("DDL column definition is invalid");
		}
		const auto &ct = StructType::GetChildTypes(col.type());
		const auto &cv = StructValue::GetChildren(col);
		const vector<string> col_names = {"name",     "type",         "length",        "precision",  "scale",
		                                  "not_null", "default_kind", "default_value", "primary_key"};
		if (ct.size() != col_names.size()) {
			throw BinderException("DDL column fields are invalid");
		}
		for (idx_t j = 0; j < col_names.size(); j++) {
			if (ct[j].first != col_names[j] || cv[j].IsNull()) {
				throw BinderException("DDL column fields are invalid");
			}
		}
		if (cv[2].type().id() != LogicalTypeId::INTEGER || cv[3].type().id() != LogicalTypeId::INTEGER ||
		    cv[4].type().id() != LogicalTypeId::INTEGER || cv[5].type().id() != LogicalTypeId::BOOLEAN ||
		    cv[8].type().id() != LogicalTypeId::BOOLEAN) {
			throw BinderException("DDL column field types are invalid");
		}
		DDLColumnSpec spec;
		spec.name = Text(cv[0]);
		spec.type = Text(cv[1]);
		spec.length = cv[2].GetValue<int32_t>();
		spec.precision = cv[3].GetValue<int32_t>();
		spec.scale = cv[4].GetValue<int32_t>();
		spec.not_null = cv[5].GetValue<bool>();
		spec.default_kind = Text(cv[6]);
		spec.default_value = Text(cv[7]);
		spec.primary_key = cv[8].GetValue<bool>();
		if (!Name(spec.name) || std::find(seen_cols.begin(), seen_cols.end(), spec.name) != seen_cols.end()) {
			throw BinderException("DDL duplicate column is invalid");
		}
		seen_cols.push_back(spec.name);
		if (spec.primary_key) {
			inline_pk_count++;
			inline_pk_col = spec.name;
		}
		if (spec.default_kind.empty() && !spec.default_value.empty()) {
			throw BinderException("DDL column default fields are invalid");
		}
		if (!spec.default_kind.empty()) {
			default_literal(spec.default_kind, spec.default_value);
			if ((spec.not_null || spec.primary_key) && spec.default_kind == "null") {
				throw BinderException("DDL NOT NULL column cannot default to NULL");
			}
		}
		string native = ColumnTypeV6(spec.type, spec.length, spec.precision, spec.scale, mysql);
		d.columns.push_back(quote(spec.name) + " " + native);
		d.column_specs.push_back(std::move(spec));
	}

	if (d.conditional && d.action != "create_table" && d.action != "drop_table" && d.action != "add_column" &&
	    d.action != "drop_column" && d.action != "create_index" && d.action != "create_unique_index" &&
	    d.action != "drop_index") {
		throw BinderException("DDL IF [NOT] EXISTS is unsupported for this action");
	}
	if ((!d.primary_key_keys.empty() || !d.primary_key_name.empty()) && d.action != "create_table") {
		throw BinderException("DDL primary key fields are invalid");
	}
	if (d.action != "create_table" && inline_pk_count > 0) {
		throw BinderException("DDL inline primary key is only valid on CREATE TABLE");
	}
	if (d.action != "create_table" && d.action != "add_column") {
		for (const auto &spec : d.column_specs) {
			if (spec.not_null || !spec.default_kind.empty() || spec.primary_key) {
				throw BinderException("DDL column attributes are unsupported for this action");
			}
		}
	}

	const bool alter_defaults = d.action == "set_default" || d.action == "drop_default" || d.action == "set_not_null" ||
	                            d.action == "drop_not_null";
	const bool col_action = alter_defaults || d.action == "rename_column" || d.action == "drop_column" ||
	                        d.action == "alter_column_type";
	if (col_action != !d.column_name.empty()) {
		throw BinderException("DDL column fields are invalid");
	}
	if (d.action != "set_default" && (!d.default_kind.empty() || !d.default_value.empty())) {
		throw BinderException("DDL default fields are invalid");
	}
	if (d.action != "add_constraint" &&
	    (!d.constraint_kind.empty() || !d.constraint_keys.empty() || !d.check_column.empty() ||
	     !d.check_operator.empty() || !d.check_value.empty() || !d.reference_schema.empty() ||
	     !d.reference_table.empty() || !d.reference_keys.empty())) {
		throw BinderException("DDL constraint fields are invalid");
	}
	if ((d.action == "add_constraint" || d.action == "drop_constraint") != !d.constraint_name.empty()) {
		throw BinderException("DDL constraint name is invalid");
	}
	if (d.action != "truncate" && !d.identity_mode.empty()) {
		throw BinderException("DDL identity field is invalid");
	}

	const auto target = quote(d.schema) + "." + quote(d.name);
	const auto alter = "ALTER TABLE " + string(mysql ? "" : "ONLY ") + target;

	auto format_col_def = [&](const DDLColumnSpec &spec, bool in_pk) {
		string def = quote(spec.name) + " " + ColumnTypeV6(spec.type, spec.length, spec.precision, spec.scale, mysql);
		if (spec.not_null || in_pk) {
			def += " NOT NULL";
			if (!spec.default_kind.empty()) {
				def += " DEFAULT " + default_literal(spec.default_kind, spec.default_value);
			}
		} else if (!spec.default_kind.empty()) {
			if (spec.default_kind == "null") {
				def += " NULL DEFAULT NULL";
			} else {
				if (mysql && spec.type == "timestamptz") {
					def += " NULL";
				}
				def += " DEFAULT " + default_literal(spec.default_kind, spec.default_value);
			}
		} else if (mysql && spec.type == "timestamptz") {
			// Avoid MySQL 5.7 implicit NOT NULL DEFAULT CURRENT_TIMESTAMP promotion.
			def += " NULL DEFAULT NULL";
		}
		return def;
	};

	if (d.action == "create_table") {
		if (d.column_specs.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() || !d.new_name.empty() ||
		    inline_pk_count > 1 || (inline_pk_count > 0 && !d.primary_key_keys.empty())) {
			throw BinderException("DDL create table options are invalid");
		}
		vector<string> effective_pk = d.primary_key_keys;
		if (inline_pk_count == 1) {
			effective_pk.push_back(inline_pk_col);
		}
		for (const auto &pk_col : effective_pk) {
			auto it = std::find_if(d.column_specs.begin(), d.column_specs.end(),
			                       [&](const DDLColumnSpec &s) { return s.name == pk_col; });
			if (it == d.column_specs.end() || it->default_kind == "null") {
				throw BinderException("DDL primary key references invalid column");
			}
		}
		if (!d.primary_key_name.empty()) {
			if (effective_pk.empty() || (mysql && d.primary_key_name != "PRIMARY")) {
				throw BinderException("DDL primary key name cannot be preserved");
			}
		}
		vector<string> col_defs;
		for (const auto &spec : d.column_specs) {
			bool in_pk = std::find(effective_pk.begin(), effective_pk.end(), spec.name) != effective_pk.end();
			col_defs.push_back(format_col_def(spec, in_pk));
		}
		if (!effective_pk.empty()) {
			string pk_clause = (!mysql && !d.primary_key_name.empty())
			                       ? ("CONSTRAINT " + quote(d.primary_key_name) + " PRIMARY KEY ")
			                       : "PRIMARY KEY ";
			col_defs.push_back(pk_clause + keysSQL(effective_pk));
		}
		d.sql = "CREATE TABLE " + target + " (" + StringUtil::Join(col_defs, ",") + ")";
		if (mysql) {
			d.sql += " ENGINE=InnoDB";
		}
	} else if (d.action == "drop_table") {
		if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() || !d.new_name.empty()) {
			throw BinderException("DDL drop table options are invalid");
		}
		d.sql = "DROP TABLE " + target + (mysql ? "" : " RESTRICT");
	} else if (d.action == "create_index" || d.action == "create_unique_index") {
		if (!d.columns.empty() || d.keys.empty() || d.index.empty() || !d.owner.empty() || !d.new_name.empty()) {
			throw BinderException("DDL create index options are invalid");
		}
		d.sql = string(d.action == "create_unique_index" ? "CREATE UNIQUE INDEX " : "CREATE INDEX ") + quote(d.index) +
		        (mysql ? " USING BTREE ON " : " ON ") + target + (mysql ? " " : " USING btree ") + keysSQL(d.keys);
	} else if (d.action == "drop_index") {
		if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.new_name.empty() ||
		    (mysql ? (d.owner.empty() && !d.conditional) : !d.owner.empty())) {
			throw BinderException("DDL drop index options are invalid");
		}
		if (!mysql) {
			d.sql = "DROP INDEX " + target + " RESTRICT";
		} else if (!d.owner.empty()) {
			d.sql = "DROP INDEX " + quote(d.name) + " ON " + quote(d.schema) + "." + quote(d.owner);
		}
	} else if (d.action == "rename_table") {
		if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() || d.new_name.empty() ||
		    d.new_name == d.name) {
			throw BinderException("DDL rename table options are invalid");
		}
		d.sql = mysql ? "RENAME TABLE " + target + " TO " + quote(d.schema) + "." + quote(d.new_name)
		              : "ALTER TABLE " + target + " RENAME TO " + quote(d.new_name);
	} else if (d.action == "add_column") {
		if (d.column_specs.size() != 1 || !d.keys.empty() || !d.index.empty() || !d.owner.empty() ||
		    !d.new_name.empty()) {
			throw BinderException("DDL add column options are invalid");
		}
		d.sql = alter + " ADD COLUMN " + format_col_def(d.column_specs[0], false);
	} else if (d.action == "drop_column") {
		if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() || !d.new_name.empty()) {
			throw BinderException("DDL drop column options are invalid");
		}
		d.sql = alter + " DROP COLUMN " + quote(d.column_name) + (mysql ? "" : " RESTRICT");
	} else if (d.action == "rename_column") {
		if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() || d.new_name.empty() ||
		    d.new_name == d.column_name) {
			throw BinderException("DDL rename column options are invalid");
		}
		d.sql = alter + " RENAME COLUMN " + quote(d.column_name) + " TO " + quote(d.new_name);
	} else if (d.action == "alter_column_type") {
		if (d.column_specs.size() != 1 || d.column_specs[0].name != d.column_name || !d.keys.empty() ||
		    !d.index.empty() || !d.owner.empty() || !d.new_name.empty()) {
			throw BinderException("DDL alter column type options are invalid");
		}
		if (!mysql) {
			d.sql = alter + " ALTER COLUMN " + quote(d.column_name) + " TYPE " +
			        ColumnTypeV6(d.column_specs[0].type, d.column_specs[0].length, d.column_specs[0].precision,
			                     d.column_specs[0].scale, false);
		}
	} else if (d.action == "rename_index") {
		if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || d.new_name.empty() ||
		    d.new_name == d.name || (mysql ? d.owner.empty() : !d.owner.empty())) {
			throw BinderException("DDL rename index options are invalid");
		}
		d.sql = mysql ? "ALTER TABLE " + quote(d.schema) + "." + quote(d.owner) + " RENAME INDEX " + quote(d.name) +
		                    " TO " + quote(d.new_name)
		              : "ALTER INDEX " + target + " RENAME TO " + quote(d.new_name);
	} else if (d.action == "set_default") {
		if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() || !d.new_name.empty()) {
			throw BinderException("DDL set default options are invalid");
		}
		d.sql = alter + " ALTER COLUMN " + quote(d.column_name) + " SET DEFAULT " +
		        default_literal(d.default_kind, d.default_value);
	} else if (d.action == "drop_default") {
		if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() || !d.new_name.empty()) {
			throw BinderException("DDL drop default options are invalid");
		}
		d.sql = alter + " ALTER COLUMN " + quote(d.column_name) + " DROP DEFAULT";
	} else if (d.action == "set_not_null" || d.action == "drop_not_null") {
		if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() || !d.new_name.empty()) {
			throw BinderException("DDL nullability options are invalid");
		}
		if (!mysql) {
			d.sql = alter + " ALTER COLUMN " + quote(d.column_name) +
			        (d.action == "set_not_null" ? " SET NOT NULL" : " DROP NOT NULL");
		}
	} else if (d.action == "add_constraint") {
		if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() || !d.new_name.empty()) {
			throw BinderException("DDL add constraint options are invalid");
		}
		string definition;
		if (d.constraint_kind == "primary_key" || d.constraint_kind == "unique" || d.constraint_kind == "foreign_key") {
			if (d.constraint_keys.empty() || !d.check_column.empty() || !d.check_operator.empty() ||
			    !d.check_value.empty()) {
				throw BinderException("DDL constraint keys are invalid");
			}
			definition = (d.constraint_kind == "primary_key" ? "PRIMARY KEY "
			              : d.constraint_kind == "unique"    ? "UNIQUE "
			                                                 : "FOREIGN KEY ") +
			             keysSQL(d.constraint_keys);
			if (d.constraint_kind == "foreign_key") {
				if (d.reference_schema != d.schema || !Name(d.reference_table) ||
				    d.reference_keys.size() != d.constraint_keys.size()) {
					throw BinderException("DDL reference is invalid");
				}
				definition += " REFERENCES " + quote(d.schema) + "." + quote(d.reference_table) + " " +
				              keysSQL(d.reference_keys) + " ON UPDATE NO ACTION ON DELETE NO ACTION";
			} else if (!d.reference_schema.empty() || !d.reference_table.empty() || !d.reference_keys.empty()) {
				throw BinderException("DDL reference fields are invalid");
			}
			if (mysql && d.constraint_kind == "primary_key" && d.constraint_name != "PRIMARY") {
				throw BinderException("DDL primary key name cannot be preserved");
			}
		} else if (d.constraint_kind == "check") {
			if (!d.constraint_keys.empty() || !d.reference_schema.empty() || !d.reference_table.empty() ||
			    !d.reference_keys.empty() || !Name(d.check_column) || !integer(d.check_value)) {
				throw BinderException("DDL CHECK fields are invalid");
			}
			if (d.check_operator != "=" && d.check_operator != "<>" && d.check_operator != "<" &&
			    d.check_operator != ">" && d.check_operator != "<=" && d.check_operator != ">=") {
				throw BinderException("DDL CHECK operator is invalid");
			}
			definition = "CHECK (" + quote(d.check_column) + " " + d.check_operator + " " + d.check_value + ")";
		} else {
			throw BinderException("DDL constraint kind is invalid");
		}
		d.sql = mysql && d.constraint_kind == "primary_key"
		            ? alter + " ADD " + definition
		            : alter + " ADD CONSTRAINT " + quote(d.constraint_name) + " " + definition;
	} else if (d.action == "drop_constraint") {
		if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() || !d.new_name.empty()) {
			throw BinderException("DDL drop constraint options are invalid");
		}
		d.sql = alter + " DROP CONSTRAINT " + quote(d.constraint_name) + (mysql ? "" : " RESTRICT");
	} else if (d.action == "truncate") {
		if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() || !d.new_name.empty()) {
			throw BinderException("DDL truncate options are invalid");
		}
		if (d.identity_mode != "" && d.identity_mode != "continue" && d.identity_mode != "restart") {
			throw BinderException("DDL identity mode is invalid");
		}
		if (mysql && d.identity_mode != "restart") {
			throw BinderException("DDL cannot preserve MySQL AUTO_INCREMENT");
		}
		d.sql = "TRUNCATE TABLE " + string(mysql ? "" : "ONLY ") + target +
		        (mysql                          ? ""
		         : d.identity_mode == "restart" ? " RESTART IDENTITY RESTRICT"
		                                        : " CONTINUE IDENTITY RESTRICT");
	} else if (d.action == "reindex") {
		if (mysql) {
			throw BinderException("DDL REINDEX is not supported");
		}
		if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() || !d.new_name.empty()) {
			throw BinderException("DDL reindex options are invalid");
		}
		d.sql = "REINDEX INDEX " + target;
	} else {
		throw BinderException("DDL action is unsupported");
	}
	if (d.sql.size() > 65536) {
		throw BinderException("DDL statement limit exceeded");
	}
	return d;
}

} // namespace duckdb
