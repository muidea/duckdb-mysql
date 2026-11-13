#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "guarded_crud_deadline.hpp"
#include "guarded_source_error.hpp"
#include "guarded_ddl_intent.hpp"
#include "mysql_column_rename.hpp"
#include "storage/mysql_catalog.hpp"

namespace duckdb {
namespace {
struct Data : TableFunctionData {
	MySQLCatalog &catalog;
	GuardedDDLIntent intent;
    bool diagnostics = false;
	Data(MySQLCatalog &c, GuardedDDLIntent d) : catalog(c), intent(std::move(d)) {
	}
};
struct State : GlobalTableFunctionState {
	bool done = false;
};
static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<State>();
}
static unique_ptr<FunctionData> BindContract(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types,
                                             vector<string> &names, int32_t contract) {
	auto intent = GuardedDDLIntent::Bind(in.inputs, true, contract);
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, GuardedDDLIntent::Text(in.inputs[0]));
	if (!db || db->GetCatalog().GetCatalogType() != "mysql")
		throw BinderException("DDL catalog is unavailable");
	names = {"operation_id", "status", "sqlstate", "stage", "warning_count"};
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::BIGINT};
    auto data = make_uniq<Data>(db->GetCatalog().Cast<MySQLCatalog>(), std::move(intent));
    auto diag=in.named_parameters.find("source_diagnostics");
    data->diagnostics=diag!=in.named_parameters.end() && !diag->second.IsNull() && diag->second.GetValue<bool>();
    if(data->diagnostics) { names.push_back("source_error");types.push_back(LogicalType::VARCHAR); }
    return std::move(data);
}
static unique_ptr<FunctionData> Bind(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types,
                                     vector<string> &names) {
	return BindContract(ctx, in, types, names, 2);
}
static unique_ptr<FunctionData> BindV3(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types,
                                       vector<string> &names) {
	return BindContract(ctx, in, types, names, 3);
}
static unique_ptr<FunctionData> BindV4(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types,
                                       vector<string> &names) {
	return BindContract(ctx, in, types, names, 4);
}
static unique_ptr<FunctionData> BindV5(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types,
                                       vector<string> &names) {
	return BindContract(ctx, in, types, names, 5);
}
static string Scalar(MYSQL *raw, const string &sql) {
	if (mysql_real_query(raw, sql.c_str(), sql.size()))
		throw IOException("DDL identity query failed");
	unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(mysql_store_result(raw), mysql_free_result);
	if (!result || mysql_num_rows(result.get()) != 1 || mysql_num_fields(result.get()) != 1)
		throw IOException("DDL identity result unavailable");
	auto row = mysql_fetch_row(result.get());
	if (!row || !row[0])
		throw IOException("DDL identity result unavailable");
	return row[0];
}
// Feature thresholds select known MySQL syntax/semantics; there is no upper
// version whitelist. Catalog presence independently proves CHECK enforcement.
static bool VersionAtLeast(const string &version,int major,int minor,int patch) {
    vector<int> parts;idx_t pos=0;
    for(int i=0;i<3;i++) {
        if(pos>=version.size() || version[pos]<'0' || version[pos]>'9') return false;
        int value=0;
        while(pos<version.size() && version[pos]>='0' && version[pos]<='9') { if(value>100000) return false;value=value*10+version[pos++]-'0'; }
        parts.push_back(value);if(i<2 && (pos>=version.size() || version[pos++]!='.')) return false;
    }
    return parts>=vector<int>{major,minor,patch};
}
static string SessionReadOnly(MYSQL *raw) {
    const string sql="SHOW SESSION VARIABLES WHERE Variable_name IN ('transaction_read_only','tx_read_only')";
    if(mysql_real_query(raw,sql.c_str(),sql.size())) throw IOException("DDL session capability unavailable");
    unique_ptr<MYSQL_RES,decltype(&mysql_free_result)> result(mysql_store_result(raw),mysql_free_result);
    if(!result || mysql_num_fields(result.get())!=2 || mysql_num_rows(result.get())<1 || mysql_num_rows(result.get())>2) throw IOException("DDL session capability unavailable");
    string value="0";MYSQL_ROW row;
    while((row=mysql_fetch_row(result.get()))) {
        if(!row[1])throw IOException("DDL session capability unavailable");
        string flag=row[1];if(flag=="ON" || flag=="1") value="1";else if(flag!="OFF" && flag!="0")throw IOException("DDL session capability unavailable");
    }
    return value;
}
static void ExecuteControl(MYSQL *raw, const string &sql) {
	if (mysql_real_query(raw, sql.c_str(), sql.size())) {
		throw IOException("DDL source control failed");
	}
}
static string MetadataLiteral(const string &value) {
	// Independent of NO_BACKSLASH_ESCAPES: this predicate is also used after
	// enabling 5.7's SHOW CREATE string escape convention for CHANGE.
	static const char hex[] = "0123456789abcdef";
	string out = "CONVERT(X'";
	for (unsigned char c : value) {
		out += hex[c >> 4];
		out += hex[c & 15];
	}
	return out + "' USING utf8mb4)";
}
static string ShowCreate(MYSQL *raw, const string &target) {
	ExecuteControl(raw, "SHOW CREATE TABLE " + target);
	unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(mysql_store_result(raw), mysql_free_result);
	if (!result || mysql_num_fields(result.get()) != 2 || mysql_num_rows(result.get()) != 1 ||
	    mysql_warning_count(raw) != 0) {
		throw std::invalid_argument("DDL source definition unavailable");
	}
	auto row = mysql_fetch_row(result.get());
	auto lengths = mysql_fetch_lengths(result.get());
	if (!row || !lengths || !row[1] || lengths[1] > MySQLRenameDefinition::MAX_BYTES) {
		throw std::invalid_argument("DDL source definition incomplete");
	}
	return string(row[1], lengths[1]);
}
static string ResolveColumn(MYSQL *raw, const string &target, const string &column) {
	// Let the server resolve identifier case rules. I_S's text collation is
	// not an identifier comparator (it can also ignore accents).
	ExecuteControl(raw, "SELECT " + KeywordHelper::WriteQuoted(column, '`') + " FROM " + target + " LIMIT 0");
	unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(mysql_store_result(raw), mysql_free_result);
	if (!result || mysql_num_fields(result.get()) != 1) {
		throw std::invalid_argument("DDL source column unavailable");
	}
	auto field = mysql_fetch_field_direct(result.get(), 0);
	if (!field || !field->org_name || !field->org_name_length) {
		throw std::invalid_argument("DDL source column identity unavailable");
	}
	return string(field->org_name, field->org_name_length);
}
static string DefaultHex(MYSQL *raw, const string &schema, const string &table, const string &column,
                         const string &charset) {
	const auto target = KeywordHelper::WriteQuoted(schema, '`') + "." + KeywordHelper::WriteQuoted(table, '`');
	const auto expression = "SELECT HEX(DEFAULT(" + KeywordHelper::WriteQuoted(column, '`') + ")) FROM " + target;
	for (const auto &query : {expression + " LIMIT 1",
	                          expression + " RIGHT JOIN (SELECT 1) AS fqe_default_probe ON FALSE LIMIT 1"}) {
		ExecuteControl(raw, query);
		unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(mysql_store_result(raw), mysql_free_result);
		if (!result || mysql_num_fields(result.get()) != 1 || mysql_num_rows(result.get()) > 1) {
			throw IOException("DDL source default result unavailable");
		}
		auto row = mysql_fetch_row(result.get());
		if (row && row[0]) {
			return row[0];
		}
	}
	// On an empty table DEFAULT(NOT NULL field) can be NULL on an outer
	// join. COM_FIELD_LIST reads the real default record instead. Connector/C
	// truncates this deprecated API's def at NUL and always sets def_length=0;
	// prove there is no NUL before using its terminated text. Never guess.
	if (charset.empty() || charset.size() > 64) {
		throw std::invalid_argument("DDL source default character set unavailable");
	}
	for (auto c : charset) {
		if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_') {
			throw std::invalid_argument("DDL source default character set unavailable");
		}
	}
	if (Scalar(raw, "SELECT LOCATE(X'00',CAST(CONVERT(COLUMN_DEFAULT USING " + charset +
	                    ") AS BINARY)) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=" +
	                    MetadataLiteral(schema) + " AND TABLE_NAME=" + MetadataLiteral(table) +
	                    " AND BINARY COLUMN_NAME=BINARY " + MetadataLiteral(column)) != "0") {
		throw std::invalid_argument("DDL empty table default bytes cannot be certified");
	}
	ExecuteControl(raw, "SET SESSION character_set_results=binary");
	unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(mysql_list_fields(raw, table.c_str(), nullptr),
	                                                        mysql_free_result);
	if (!result) {
		throw IOException("DDL source default record unavailable");
	}
	if (mysql_num_fields(result.get()) > 4096) {
		throw std::invalid_argument("DDL source default record limit exceeded");
	}
	string value;
	bool found = false;
	for (unsigned int i = 0; i < mysql_num_fields(result.get()); i++) {
		auto field = mysql_fetch_field_direct(result.get(), i);
		if (!field || !field->name || string(field->name, field->name_length) != column) {
			continue;
		}
		if (found || !field->def) {
			throw std::invalid_argument("DDL source default record incomplete");
		}
		const auto length = strnlen(field->def, MySQLRenameDefinition::MAX_BYTES / 2 + 1);
		if (length > MySQLRenameDefinition::MAX_BYTES / 2) {
			throw std::invalid_argument("DDL source default record limit exceeded");
		}
		found = true;
		static const char hex[] = "0123456789ABCDEF";
		for (size_t j = 0; j < length; j++) {
			auto c = static_cast<unsigned char>(field->def[j]);
			value += hex[c >> 4];
			value += hex[c & 15];
		}
	}
	if (!found) {
		throw std::invalid_argument("DDL source default record missing");
	}
	ExecuteControl(raw, "SET SESSION character_set_results=utf8mb4");
	return value;
}
static void Scan(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
	auto &state = in.global_state->Cast<State>();
	if (state.done)
		return;
	state.done = true;
	auto &d = in.bind_data->Cast<Data>();
	string status = "CONFIRMED_NOT_EXECUTED", error, stage = "source_identity";
	GuardedSourceError source;
	MySQLPooledConnection pooled;
	unique_ptr<GuardedCRUDDeadline> deadline;
	bool sent = false;
	int64_t warnings = 0;
	try {
		if (ctx.IsInterrupted())
			throw InterruptException();
		const bool mariadb = d.catalog.GetBackendCapabilities().kind == MySQLBackendKind::MARIADB_STANDARD;
		if (!mariadb && d.catalog.GetBackendCapabilities().kind != MySQLBackendKind::MYSQL_STANDARD) {
			error = "0A000";
			throw IOException("DDL source unsupported");
		}
		pooled = d.catalog.GetConnectionPool().Acquire(dbconnector::pool::AcquireMode::TRY);
		auto raw = pooled.GetConnection().GetConn();
		deadline =
		    make_uniq<GuardedCRUDDeadline>(ctx, mysql_get_socket(raw), d.intent.contract_version == 4 ? 120 : 30);
		if (raw->server_status & SERVER_STATUS_IN_TRANS)
			throw IOException("DDL session is not idle");
		if (mysql_reset_connection(raw))
			throw IOException("DDL session reset failed");
		auto version = Scalar(raw, "SELECT VERSION()");
		auto comment = Scalar(raw, "SELECT @@version_comment");
		bool distribution =
		    ((comment == "Ubuntu" || comment == "(Ubuntu)") && version.find("ubuntu") != string::npos) ||
		    ((comment == "Debian" || comment == "(Debian)") && version.find("debian") != string::npos);
		if (((mariadb && comment.find("MariaDB") == string::npos) ||
		     (!mariadb && !distribution && comment.find("MySQL") == string::npos)) ||
		    Scalar(raw, "SELECT DATABASE()") != d.intent.database) {
			error = "0A000";
			throw IOException("DDL source not certified");
		}
		const bool change_column = d.intent.action == "rename_column" &&
		                           (mariadb ? !VersionAtLeast(version, 10, 5, 0) : !VersionAtLeast(version, 8, 0, 0));
		if (change_column && !mariadb && !VersionAtLeast(version, 5, 7, 0)) {
			error = "0A000";
			throw IOException("DDL column rename has no equivalent native syntax");
		}
		if (d.intent.action == "add_constraint" && d.intent.constraint_kind == "check" &&
		    (mariadb ? !VersionAtLeast(version, 10, 2, 1)
		             : (!VersionAtLeast(version,8,0,16) || Scalar(raw,"SELECT CAST(COUNT(*) AS CHAR) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='information_schema' AND TABLE_NAME='TABLE_CONSTRAINTS' AND COLUMN_NAME='ENFORCED'")!="1"))) {
			error = "0A000";
			throw IOException("DDL enforced CHECK is unavailable");
		}
		if (Scalar(raw, "SELECT CAST(@@read_only AS CHAR)") != "0" ||
		    SessionReadOnly(raw) != "0") {
			error = "25006";
			throw IOException("DDL source read only");
		}
		const string settings =
		    "SET SESSION "
		    "lock_wait_timeout=5,innodb_lock_wait_timeout=5,sql_mode='STRICT_ALL_TABLES,NO_ZERO_"
		    "DATE,NO_ZERO_IN_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION,NO_BACKSLASH_ESCAPES'";
		if (mysql_real_query(raw, settings.c_str(), settings.size()) || mysql_set_character_set(raw, "utf8mb4"))
			throw IOException("DDL session settings failed");
		MySQLRenameDefinition definition;
		string canonical_column, default_hex, default_charset;
		bool string_default = false, nullable_null_default = false;
		const auto target = KeywordHelper::WriteQuoted(d.intent.schema, '`') + "." +
		                    KeywordHelper::WriteQuoted(d.intent.name, '`');
		if (change_column) {
			stage = "source_lock";
			// 5.7 SHOW CREATE escapes backslashes even with NO_BACKSLASH_ESCAPES
			// set. Match its output convention when executing the retained text.
			ExecuteControl(raw, "SET SESSION autocommit=0,sql_quote_show_create=1,"
			                    "sql_mode='STRICT_ALL_TABLES,NO_ZERO_DATE,NO_ZERO_IN_DATE,"
			                    "ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION'");
			// Disable reconnect explicitly: losing this connection must never
			// silently lose the lock and continue with a stale definition.
			bool reconnect = false;
			if (mysql_options(raw, MYSQL_OPT_RECONNECT, &reconnect)) {
				throw IOException("DDL reconnect protection unavailable");
			}
			ExecuteControl(raw, "LOCK TABLES " + target + " WRITE");
			stage = "source_definition";
		}
		if (ctx.IsInterrupted())
			throw InterruptException();
		if (d.intent.contract_version >= 3) {
			const string table = d.intent.action == "rename_index" ? d.intent.owner : d.intent.name;
			const auto predicate = "TABLE_SCHEMA=" + MetadataLiteral(d.intent.schema) + " AND TABLE_NAME=" + MetadataLiteral(table);
			const bool present =
			    Scalar(raw, "SELECT count(*) FROM information_schema.tables WHERE " + predicate) != "0";
			if (present && (Scalar(raw, "SELECT count(*) FROM information_schema.tables WHERE " + predicate +
			                                " AND TABLE_TYPE='BASE TABLE' AND ENGINE='InnoDB'") != "1" ||
			                Scalar(raw, "SELECT count(*) FROM information_schema.partitions WHERE " + predicate +
			                                " AND PARTITION_NAME IS NOT NULL") != "0")) {
				error = "0A000";
				throw IOException("DDL ordinary InnoDB table scope is unavailable");
			}
		}
		auto native_sql = d.intent.sql;
		if (change_column) {
			canonical_column = ResolveColumn(raw, target, d.intent.column_name);
			definition = MySQLRenameDefinition::Parse(ShowCreate(raw, target), d.intent.name);
			auto execution_definition = definition;
			const auto properties = Scalar(raw, "SELECT CONCAT(COALESCE(CHARACTER_SET_NAME,'binary'),':',DATA_TYPE,':',"
			                                   "IF(COLUMN_DEFAULT IS NULL,'0','1'),':',IF(GENERATION_EXPRESSION='','0','1'),':',IS_NULLABLE) FROM information_schema.COLUMNS WHERE "
			                                   "TABLE_SCHEMA=" + MetadataLiteral(d.intent.schema) + " AND TABLE_NAME=" +
			                                   MetadataLiteral(d.intent.name) + " AND BINARY COLUMN_NAME=BINARY " +
			                                   MetadataLiteral(canonical_column));
			auto parts = StringUtil::Split(properties, ':');
			if (parts.size() != 5) {
				throw std::invalid_argument("DDL source default metadata incomplete");
			}
			nullable_null_default = parts[2] == "0" && parts[3] == "0" && parts[4] == "YES";
			if (parts[3] == "1") {
				// SHOW CREATE can replace each byte of a four-byte literal in
				// a generated expression with '?'. I_S may truncate it instead,
				// so neither text is an independent proof of that expression.
				for (const auto &column : definition.columns) {
					if (column.name == canonical_column && column.definition.find('?') != string::npos) {
						throw std::invalid_argument("DDL source generated expression cannot be certified");
					}
				}
			}
			// 5.7 serializes ENUM/SET members through utf8mb3 as well. A '?'
			// may be a literal member or a substituted four-byte character;
			// neither SHOW nor I_S can distinguish them. Refuse before CHANGE.
			if (parts[0] == "utf8mb4" && (parts[1] == "enum" || parts[1] == "set") &&
			    Scalar(raw, "SELECT LOCATE('?',COLUMN_TYPE) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=" +
			                    MetadataLiteral(d.intent.schema) + " AND TABLE_NAME=" + MetadataLiteral(d.intent.name) +
			                    " AND BINARY COLUMN_NAME=BINARY " + MetadataLiteral(canonical_column)) != "0") {
				throw std::invalid_argument("DDL source enum members cannot be certified");
			}
			if (parts[2] == "1" && (parts[1] == "char" || parts[1] == "varchar" || parts[1] == "enum" ||
			                        parts[1] == "set" || parts[1] == "binary" || parts[1] == "varbinary")) {
				string_default = true;
				default_charset = parts[0];
				default_hex = DefaultHex(raw, d.intent.schema, d.intent.name, canonical_column, default_charset);
				execution_definition.StringDefault(canonical_column, parts[0], default_hex);
			}
			native_sql = execution_definition.Change(d.intent.schema, d.intent.name, canonical_column, d.intent.new_name);
			if (native_sql.size() > 65536) {
				throw std::invalid_argument("DDL generated definition limit exceeded");
			}
		}
		if (d.intent.action == "drop_constraint") {
			auto literal = [](const string &v) {
				return "'" + StringUtil::Replace(v, "'", "''") + "'";
			};
			auto kind = Scalar(raw, "SELECT COALESCE(MAX(CONSTRAINT_TYPE),'MISSING') FROM "
			                        "information_schema.table_constraints WHERE CONSTRAINT_SCHEMA=" +
			                            literal(d.intent.schema) + " AND TABLE_NAME=" + literal(d.intent.name) +
			                            " AND CONSTRAINT_NAME=" + literal(d.intent.constraint_name));
			const auto target =
			    KeywordHelper::WriteQuoted(d.intent.schema, '`') + "." + KeywordHelper::WriteQuoted(d.intent.name, '`');
			const auto name = KeywordHelper::WriteQuoted(d.intent.constraint_name, '`');
			if (kind == "PRIMARY KEY")
				native_sql = "ALTER TABLE " + target + " DROP PRIMARY KEY";
			else if (kind == "UNIQUE")
				native_sql = "ALTER TABLE " + target + " DROP INDEX " + name;
			else if (kind == "FOREIGN KEY")
				native_sql = "ALTER TABLE " + target + " DROP FOREIGN KEY " + name;
			else if (kind == "CHECK" && (mariadb ? VersionAtLeast(version, 10, 2, 1) : VersionAtLeast(version,8,0,16)))
				native_sql = mariadb ? "ALTER TABLE " + target + " DROP CONSTRAINT " + name
				                     : "ALTER TABLE " + target + " DROP CHECK " + name;
			// If no supported catalog kind is visible, send the original
			// native DROP CONSTRAINT form and retain the source's own error.
		}
		stage = "source_execute";
		if (ctx.IsInterrupted()) {
			throw InterruptException();
		}
		sent = true;
		if (mysql_real_query(raw, native_sql.c_str(), native_sql.size())) {
            source.product="mysql"; source.state=mysql_sqlstate(raw); source.code=std::to_string(mysql_errno(raw)); source.message=mysql_error(raw);
			auto code = mysql_errno(raw);
			const char *s = mysql_sqlstate(raw);
			error = s && strlen(s) == 5 ? s : "HY000";
			// Unknown/native partial effects are not reported as an unsent request.
			switch (code) {
			case 1054:
			case 1064:
			case 1067:
			case 1171:
			case 1364:
			case 1048:
			case 1062:
			case 1138:
			case 3819:
			case 3823:
			case 1050:
			case 1051:
			case 1061:
			case 1060:
			case 1091:
			case 1142:
			case 1146:
			case 1217:
			case 1451:
			case 3108: // dependent generated column: rejected before changing the definition
				status = "CONFIRMED_NOT_EXECUTED";
				sent = false;
				break;
			default:
				status = "UNKNOWN";
			}
			throw IOException("DDL native execution failed");
		}
		if (raw->server_status & SERVER_STATUS_IN_TRANS) {
			status = "UNKNOWN";
			throw IOException("DDL commit outcome unavailable");
		}
		status = "COMMITTED";
		warnings = mysql_warning_count(raw);
		stage = warnings ? "source_warning" : "";
		if (change_column && !warnings) {
			stage = "source_verify";
			auto after = MySQLRenameDefinition::Parse(ShowCreate(raw, target), d.intent.name);
			if (!definition.Preserved(after, canonical_column, d.intent.new_name, nullable_null_default) ||
			    (string_default && DefaultHex(raw, d.intent.schema, d.intent.name, d.intent.new_name, default_charset) != default_hex)) {
				error = "XX000";
				throw IOException("DDL committed column attributes differ from source definition");
			}
			stage.clear();
		}
	} catch (const std::invalid_argument &) {
		// Parsing/size failures have no source errno; distinguish these from
		// transport failures. A confirmed commit is never rewritten as unsent.
		error = status == "COMMITTED" ? "XX000" : "0A000";
	} catch (...) {
        if(source.message.empty() && pooled) {
            auto raw=pooled.GetConnection().GetConn();
            if(mysql_errno(raw)) {source.product="mysql";source.state=mysql_sqlstate(raw);source.code=std::to_string(mysql_errno(raw));source.message=mysql_error(raw);}
        }
		if (error.empty())
			error = ctx.IsInterrupted() ? "57014" : (!source.state.empty() ? source.state : "08006");
		if (sent && status != "COMMITTED")
			status = "UNKNOWN";
	}
	if (pooled) {
		deadline.reset();
		// Close releases table locks even after failure/cancellation; never
		// return a lock-bearing session to the pool or retry its DDL.
		pooled.Invalidate();
	}
	out.SetCardinality(1);
    if(d.diagnostics) out.SetValue(5,0,Value(source.JSON()));
	out.SetValue(0, 0, Value(d.intent.operation));
	out.SetValue(1, 0, Value(status));
	out.SetValue(2, 0, Value(error));
	out.SetValue(3, 0, Value(stage));
	out.SetValue(4, 0, Value::BIGINT(warnings));
}
} // namespace
void RegisterMySQLGuardedDDL(ExtensionLoader &loader) {
  auto register_function = [&](TableFunction function) {function.named_parameters["source_diagnostics"]=LogicalType::BOOLEAN;loader.RegisterFunction(function);};
  register_function(TableFunction(
	  "mysql_guarded_ddl_v5",
	  {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY}, Scan,
	  BindV5, Init));
	register_function(TableFunction(
	    "mysql_guarded_ddl_v2",
	    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
	    Scan, Bind, Init));
	register_function(TableFunction(
	    "mysql_guarded_ddl_v3",
	    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
	    Scan, BindV3, Init));
	register_function(TableFunction(
	    "mysql_guarded_ddl_v4",
	    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
	    Scan, BindV4, Init));
}
} // namespace duckdb
