#pragma once
#include <cerrno>
#include <cstdlib>

namespace duckdb {
inline GuardedDDLIntent GuardedDDLIntent::BindV4(const vector<Value> &input, bool mysql) {
	if (input.size() != 5)
		throw BinderException("DDL structured input is invalid");
	GuardedDDLIntent d;
	d.contract_version = 4;
	d.database = Text(input[1]);
	d.schema = Text(input[2]);
	d.operation = Text(input[3]);
	if (!Name(d.database) || !Name(d.schema) || (mysql && d.database != d.schema) || d.operation.size() != 36)
		throw BinderException("DDL scope is invalid");
	for (idx_t i = 0; i < d.operation.size(); i++) {
		auto c = d.operation[i];
		if ((i == 8 || i == 13 || i == 18 || i == 23) ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			throw BinderException("DDL operation identity is invalid");
	}
	auto &v = input[4];
	if (v.IsNull() || v.type().id() != LogicalTypeId::STRUCT)
		throw BinderException("DDL input is invalid");
	const auto &types = StructType::GetChildTypes(v.type());
	const auto &f = StructValue::GetChildren(v);
	vector<string> names = {"contract_version", "target_contract",
	                        "action",           "name",
	                        "index_name",       "owner",
	                        "new_name",         "column_name",
	                        "default_kind",     "default_value",
	                        "constraint_name",  "constraint_kind",
	                        "check_column",     "check_operator",
	                        "check_value",      "reference_schema",
	                        "reference_table",  "identity_mode",
	                        "constraint_keys",  "reference_keys",
	                        "columns",          "keys"};
	if (types.size() != names.size())
		throw BinderException("DDL fields are invalid");
	for (idx_t i = 0; i < names.size(); i++)
		if (types[i].first != names[i] || f[i].IsNull())
			throw BinderException("DDL fields are invalid");
	if (f[0].type().id() != LogicalTypeId::INTEGER || f[0].GetValue<int32_t>() != 4 || Text(f[1]) != "EXECUTION_NAME")
		throw BinderException("DDL contract is unavailable");
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
	if (!Name(d.name) || !d.index.empty() || !d.owner.empty() || !d.new_name.empty())
		throw BinderException("DDL target is invalid");
	for (auto &n : {d.column_name, d.constraint_name, d.check_column, d.reference_schema, d.reference_table})
		if (!n.empty() && !Name(n))
			throw BinderException("DDL identifier is invalid");
	for (idx_t x = 18; x < 22; x++) {
		if (f[x].type().id() != LogicalTypeId::LIST)
			throw BinderException("DDL list is invalid");
		auto &items = ListValue::GetChildren(f[x]);
		if (x >= 20 && !items.empty())
			throw BinderException("DDL extraneous fields are invalid");
		if (items.size() > 32)
			throw BinderException("DDL key limit exceeded");
		vector<string> *keys = x == 18 ? &d.constraint_keys : &d.reference_keys;
		for (auto &item : items) {
			auto n = Text(item);
			if (!Name(n) || std::find(keys->begin(), keys->end(), n) != keys->end())
				throw BinderException("DDL key is invalid");
			keys->push_back(n);
		}
	}
	auto integer = [](const string &s) {
		if (s.empty() || s.size() > 20 || s.find('\0') != string::npos)
			return false;
		errno = 0;
		char *end = nullptr;
		auto n = std::strtoll(s.c_str(), &end, 10);
		return errno != ERANGE && end == s.c_str() + s.size() && std::to_string(n) == s;
	};
	auto quote = [&](const string &s) {
		return KeywordHelper::WriteQuoted(s, mysql ? '`' : '"');
	};
	auto literal = [](const string &s) {
		return "'" + StringUtil::Replace(s, "'", "''") + "'";
	};
	auto keysSQL = [&](const vector<string> &keys) {
		vector<string> out;
		for (auto &k : keys)
			out.push_back(quote(k));
		return "(" + StringUtil::Join(out, ",") + ")";
	};
	const auto target = quote(d.schema) + "." + quote(d.name);
	const auto alter = "ALTER TABLE " + string(mysql ? "" : "ONLY ") + target;
	const bool column = d.action == "set_default" || d.action == "drop_default" || d.action == "set_not_null" ||
	                    d.action == "drop_not_null";
	if (column != !d.column_name.empty())
		throw BinderException("DDL column fields are invalid");
	if (d.action != "set_default" && (!d.default_kind.empty() || !d.default_value.empty()))
		throw BinderException("DDL default fields are invalid");
	if (d.action != "add_constraint" &&
	    (!d.constraint_kind.empty() || !d.constraint_keys.empty() || !d.check_column.empty() ||
	     !d.check_operator.empty() || !d.check_value.empty() || !d.reference_schema.empty() ||
	     !d.reference_table.empty() || !d.reference_keys.empty()))
		throw BinderException("DDL constraint fields are invalid");
	if ((d.action == "add_constraint" || d.action == "drop_constraint") != !d.constraint_name.empty())
		throw BinderException("DDL constraint name is invalid");
	if (d.action != "truncate" && !d.identity_mode.empty())
		throw BinderException("DDL identity field is invalid");
	if (d.action == "set_default") {
		string value;
		if (d.default_kind == "null" && d.default_value.empty())
			value = "NULL";
		else if (d.default_kind == "integer" && integer(d.default_value))
			value = d.default_value;
		else if (d.default_kind == "string" && d.default_value.size() <= 4096 &&
		         d.default_value.find('\0') == string::npos)
			value = literal(d.default_value);
		else
			throw BinderException("DDL default literal is invalid");
		d.sql = alter + " ALTER COLUMN " + quote(d.column_name) + " SET DEFAULT " + value;
	} else if (d.action == "drop_default") {
		d.sql = alter + " ALTER COLUMN " + quote(d.column_name) + " DROP DEFAULT";
	} else if (d.action == "set_not_null" || d.action == "drop_not_null") {
		// MySQL MODIFY needs a full column definition and could overwrite attributes
		// changed concurrently. There is no equivalent attribute-only native syntax.
		if (mysql)
			throw BinderException("DDL nullability has no equivalent native syntax");
		d.sql = alter + " ALTER COLUMN " + quote(d.column_name) +
		        (d.action == "set_not_null" ? " SET NOT NULL" : " DROP NOT NULL");
	} else if (d.action == "add_constraint") {
		string definition;
		if (d.constraint_kind == "primary_key" || d.constraint_kind == "unique" || d.constraint_kind == "foreign_key") {
			if (d.constraint_keys.empty() || !d.check_column.empty() || !d.check_operator.empty() ||
			    !d.check_value.empty())
				throw BinderException("DDL constraint keys are invalid");
			definition = (d.constraint_kind == "primary_key" ? "PRIMARY KEY "
			              : d.constraint_kind == "unique"    ? "UNIQUE "
			                                                 : "FOREIGN KEY ") +
			             keysSQL(d.constraint_keys);
			if (d.constraint_kind == "foreign_key") {
				if (d.reference_schema != d.schema || !Name(d.reference_table) ||
				    d.reference_keys.size() != d.constraint_keys.size())
					throw BinderException("DDL reference is invalid");
				definition += " REFERENCES " + quote(d.schema) + "." + quote(d.reference_table) + " " +
				              keysSQL(d.reference_keys) + " ON UPDATE NO ACTION ON DELETE NO ACTION";
			} else if (!d.reference_schema.empty() || !d.reference_table.empty() || !d.reference_keys.empty())
				throw BinderException("DDL reference fields are invalid");
			if (mysql && d.constraint_kind == "primary_key" && d.constraint_name != "PRIMARY")
				throw BinderException("DDL primary key name cannot be preserved");
		} else if (d.constraint_kind == "check") {
			if (!d.constraint_keys.empty() || !d.reference_schema.empty() || !d.reference_table.empty() ||
			    !d.reference_keys.empty() || !Name(d.check_column) || !integer(d.check_value))
				throw BinderException("DDL CHECK fields are invalid");
			if (d.check_operator != "=" && d.check_operator != "<>" && d.check_operator != "<" &&
			    d.check_operator != ">" && d.check_operator != "<=" && d.check_operator != ">=")
				throw BinderException("DDL CHECK operator is invalid");
			definition = "CHECK (" + quote(d.check_column) + " " + d.check_operator + " " + d.check_value + ")";
		} else
			throw BinderException("DDL constraint kind is invalid");
		// MySQL 5.7 rejects CONSTRAINT `PRIMARY`; the contract preserves its fixed name.
        d.sql = mysql && d.constraint_kind=="primary_key" ? alter+" ADD "+definition
            : alter+" ADD CONSTRAINT "+quote(d.constraint_name)+" "+definition;
	} else if (d.action == "drop_constraint") {
		// MySQL determines its native DROP form from the constraint catalog at send.
		d.sql = alter + " DROP CONSTRAINT " + quote(d.constraint_name) + (mysql ? "" : " RESTRICT");
	} else if (d.action == "truncate") {
		if (d.identity_mode != "" && d.identity_mode != "continue" && d.identity_mode != "restart")
			throw BinderException("DDL identity mode is invalid");
		if (mysql && d.identity_mode != "restart")
			throw BinderException("DDL cannot preserve MySQL AUTO_INCREMENT");
		d.sql = "TRUNCATE TABLE " + string(mysql ? "" : "ONLY ") + target +
		        (mysql                          ? ""
		         : d.identity_mode == "restart" ? " RESTART IDENTITY RESTRICT"
		                                        : " CONTINUE IDENTITY RESTRICT");
	} else if (d.action == "reindex") {
		if (mysql)
			throw BinderException("DDL REINDEX is not supported");
		d.sql = "REINDEX INDEX " + target;
	} else
		throw BinderException("DDL action requires a different contract");
	if (d.sql.size() > 65536)
		throw BinderException("DDL statement limit exceeded");
	return d;
}
} // namespace duckdb
