#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "guarded_crud_deadline.hpp"
#include "guarded_source_error.hpp"
#include "guarded_ddl_intent.hpp"
#include "mysql_column_rename.hpp"
#include "storage/mysql_catalog.hpp"
#include <cctype>

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
                                             vector<Identifier> &names, int32_t contract) {
	auto cond = in.named_parameters.find("conditional");
	const bool conditional = cond != in.named_parameters.end() && !cond->second.IsNull() && cond->second.GetValue<bool>();
	auto intent = GuardedDDLIntent::Bind(in.inputs, true, contract, conditional);
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, Identifier(GuardedDDLIntent::Text(in.inputs[0])));
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
                                     vector<Identifier> &names) {
	if (in.inputs.size() != 5 || in.inputs[4].IsNull() || in.inputs[4].type().id() != LogicalTypeId::STRUCT) {
		throw BinderException("DDL structured input is invalid");
	}
	const auto &st = StructType::GetChildTypes(in.inputs[4].type());
	const auto &sf = StructValue::GetChildren(in.inputs[4]);
	if (st.empty() || st[0].first != "contract_version" || sf[0].IsNull() ||
	    sf[0].type().id() != LogicalTypeId::INTEGER) {
		throw BinderException("DDL contract is unavailable");
	}
	const int32_t contract = sf[0].GetValue<int32_t>();
	if (contract < 2 || contract > 6) {
		throw BinderException("DDL contract is unavailable");
	}
	return BindContract(ctx, in, types, names, contract);
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
static vector<string> ColumnValues(MYSQL *raw, const string &sql) {
	if (mysql_real_query(raw, sql.c_str(), sql.size()))
		throw IOException("DDL metadata query failed");
	unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(mysql_store_result(raw), mysql_free_result);
	if (!result || mysql_num_fields(result.get()) != 1)
		throw IOException("DDL metadata result unavailable");
	vector<string> values;
	MYSQL_ROW row;
	while ((row = mysql_fetch_row(result.get()))) {
		auto lengths = mysql_fetch_lengths(result.get());
		if (row[0] && lengths) {
			values.emplace_back(row[0], lengths[0]);
		}
	}
	return values;
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

static bool ClauseMentionsIdentifier(const string &clause, const string &identifier) {
	if (clause.empty() || identifier.empty()) {
		return false;
	}
	auto equal_ci = [&](const string &cand) { return StringUtil::CIEquals(cand, identifier); };
	auto is_ident_char = [](unsigned char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
	};
	for (size_t p = 0; p < clause.size();) {
		char ch = clause[p];
		if (ch == '\'' || ch == '"') {
			char q = ch;
			p++;
			while (p < clause.size()) {
				if (clause[p] == '\\') {
					p += 2;
				} else if (clause[p] == q) {
					p++;
					if (p < clause.size() && clause[p] == q) {
						p++;
					} else {
						break;
					}
				} else {
					p++;
				}
			}
			continue;
		}
		if (ch == '`') {
			p++;
			string cand;
			while (p < clause.size()) {
				if (clause[p] == '`') {
					if (p + 1 < clause.size() && clause[p + 1] == '`') {
						cand += '`';
						p += 2;
					} else {
						p++;
						break;
					}
				} else {
					cand += clause[p++];
				}
			}
			if (equal_ci(cand)) {
				return true;
			}
			continue;
		}
		if (is_ident_char(static_cast<unsigned char>(ch))) {
			size_t start = p;
			while (p < clause.size() && is_ident_char(static_cast<unsigned char>(clause[p]))) {
				p++;
			}
			if (equal_ci(clause.substr(start, p - start))) {
				return true;
			}
			continue;
		}
		p++;
	}
	return false;
}

static bool HasQuotedStringDefault(const MySQLRenameDefinition &def, const string &col_name) {
	for (const auto &column : def.columns) {
		if (column.name != col_name) {
			continue;
		}
		auto tokens = MySQLRenameDefinition::TokenizeTopLevel(column.definition);
		for (size_t i = 0; i + 1 < tokens.size(); i++) {
			if (tokens[i].is_word && tokens[i].upper == "DEFAULT") {
				size_t next_start = tokens[i + 1].start;
				return next_start < column.definition.size() && column.definition[next_start] == '\'';
			}
		}
	}
	return false;
}

static bool ParseTwoUInts(const string &column_type, const string &prefix, int32_t &first, int32_t &second,
                          int32_t default_val = 0) {
	auto lower = StringUtil::Lower(column_type);
	if (lower == prefix) {
		first = default_val;
		second = 0;
		return true;
	}
	if (lower.rfind(prefix + "(", 0) != 0 || lower.back() != ')') {
		return false;
	}
	auto inner = lower.substr(prefix.size() + 1, lower.size() - prefix.size() - 2);
	auto comma = inner.find(',');
	auto parse_nonneg = [](const string &s, int32_t &out) {
		if (s.empty() || s.size() > 6)
			return false;
		int64_t val = 0;
		for (char c : s) {
			if (c < '0' || c > '9')
				return false;
			val = val * 10 + (c - '0');
		}
		if (val > 100000)
			return false;
		out = static_cast<int32_t>(val);
		return true;
	};
	if (comma == string::npos) {
		second = 0;
		return parse_nonneg(inner, first);
	}
	return parse_nonneg(inner.substr(0, comma), first) && parse_nonneg(inner.substr(comma + 1), second);
}

static void CheckOrdinaryInnoDBTable(MYSQL *raw, const string &schema, const string &table, string &error) {
	const auto predicate = "TABLE_SCHEMA=" + MetadataLiteral(schema) + " AND TABLE_NAME=" + MetadataLiteral(table);
	const bool present = Scalar(raw, "SELECT count(*) FROM information_schema.tables WHERE " + predicate) != "0";
	if (present && (Scalar(raw, "SELECT count(*) FROM information_schema.tables WHERE " + predicate +
	                                " AND TABLE_TYPE='BASE TABLE' AND ENGINE='InnoDB'") != "1" ||
	                Scalar(raw, "SELECT count(*) FROM information_schema.partitions WHERE " + predicate +
	                                " AND PARTITION_NAME IS NOT NULL") != "0")) {
		error = "0A000";
		throw IOException("DDL ordinary InnoDB table scope is unavailable");
	}
}

static bool TableExists(MYSQL *raw, const string &schema, const string &table) {
	return Scalar(raw, "SELECT count(*) FROM information_schema.tables WHERE TABLE_SCHEMA=" + MetadataLiteral(schema) +
	                       " AND TABLE_NAME=" + MetadataLiteral(table)) != "0";
}

static bool ColumnExists(MYSQL *raw, const string &schema, const string &table, const string &column) {
	return Scalar(raw, "SELECT count(*) FROM information_schema.columns WHERE TABLE_SCHEMA=" + MetadataLiteral(schema) +
	                       " AND TABLE_NAME=" + MetadataLiteral(table) + " AND LOWER(COLUMN_NAME)=LOWER(" +
	                       MetadataLiteral(column) + ")") != "0";
}

static bool IndexExists(MYSQL *raw, const string &schema, const string &table, const string &index_name) {
	return Scalar(raw, "SELECT count(*) FROM information_schema.statistics WHERE TABLE_SCHEMA=" +
	                       MetadataLiteral(schema) + " AND TABLE_NAME=" + MetadataLiteral(table) +
	                       " AND LOWER(INDEX_NAME)=LOWER(" + MetadataLiteral(index_name) + ")") != "0";
}

static void CheckColumnDependencies(MYSQL *raw, const string &schema, const string &table,
                                    const string &canonical_column, bool is_alter_type, string &error) {
	const auto pred = "TABLE_SCHEMA=" + MetadataLiteral(schema) + " AND TABLE_NAME=" + MetadataLiteral(table);
	if (Scalar(raw, "SELECT count(*) FROM information_schema.STATISTICS WHERE " + pred +
	                    " AND BINARY COLUMN_NAME=BINARY " + MetadataLiteral(canonical_column)) != "0") {
		error = "2BP01";
		throw IOException("DDL column has index or key dependencies");
	}
	if (Scalar(raw, "SELECT count(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='information_schema' AND "
	                "TABLE_NAME='STATISTICS' AND COLUMN_NAME='EXPRESSION'") == "1") {
		auto exprs = ColumnValues(raw, "SELECT EXPRESSION FROM information_schema.STATISTICS WHERE " + pred +
		                                   " AND EXPRESSION IS NOT NULL");
		for (const auto &ex : exprs) {
			if (ClauseMentionsIdentifier(ex, canonical_column)) {
				error = "2BP01";
				throw IOException("DDL column has functional index dependencies");
			}
		}
	}
	if (Scalar(raw, "SELECT count(*) FROM information_schema.KEY_COLUMN_USAGE WHERE (" + pred +
	                    " AND BINARY COLUMN_NAME=BINARY " + MetadataLiteral(canonical_column) +
	                    ") OR (REFERENCED_TABLE_SCHEMA=" + MetadataLiteral(schema) +
	                    " AND REFERENCED_TABLE_NAME=" + MetadataLiteral(table) +
	                    " AND BINARY REFERENCED_COLUMN_NAME=BINARY " + MetadataLiteral(canonical_column) + ")") != "0") {
		error = "2BP01";
		throw IOException("DDL column has constraint dependencies");
	}
	if (Scalar(raw, "SELECT count(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA='information_schema' AND "
	                "TABLE_NAME='CHECK_CONSTRAINTS'") == "1") {
		auto clauses = ColumnValues(
		    raw, "SELECT cc.CHECK_CLAUSE FROM information_schema.TABLE_CONSTRAINTS tc "
		         "JOIN information_schema.CHECK_CONSTRAINTS cc ON cc.CONSTRAINT_SCHEMA=tc.CONSTRAINT_SCHEMA AND "
		         "cc.CONSTRAINT_NAME=tc.CONSTRAINT_NAME WHERE tc.CONSTRAINT_SCHEMA=" +
		             MetadataLiteral(schema) + " AND tc.TABLE_NAME=" + MetadataLiteral(table) +
		             " AND tc.CONSTRAINT_TYPE='CHECK'");
		for (const auto &clause : clauses) {
			if (ClauseMentionsIdentifier(clause, canonical_column)) {
				error = "2BP01";
				throw IOException("DDL column has CHECK constraint dependencies");
			}
		}
	}
	auto views = ColumnValues(
	    raw, "SELECT VIEW_DEFINITION FROM information_schema.VIEWS WHERE TABLE_SCHEMA=" + MetadataLiteral(schema));
	for (const auto &view_def : views) {
		if (ClauseMentionsIdentifier(view_def, table) && ClauseMentionsIdentifier(view_def, canonical_column)) {
			error = "2BP01";
			throw IOException("DDL column has view dependencies");
		}
	}
	if (Scalar(raw, "SELECT count(*) FROM information_schema.TRIGGERS WHERE EVENT_OBJECT_SCHEMA=" +
	                    MetadataLiteral(schema) + " AND EVENT_OBJECT_TABLE=" + MetadataLiteral(table)) != "0") {
		error = "2BP01";
		throw IOException("DDL column table has trigger dependencies");
	}
	auto gen_others = ColumnValues(raw, "SELECT GENERATION_EXPRESSION FROM information_schema.COLUMNS WHERE " + pred +
	                                        " AND GENERATION_EXPRESSION <> '' AND BINARY COLUMN_NAME <> BINARY " +
	                                        MetadataLiteral(canonical_column));
	for (const auto &gex : gen_others) {
		if (ClauseMentionsIdentifier(gex, canonical_column)) {
			error = "2BP01";
			throw IOException("DDL column has generated column dependencies");
		}
	}
	if (is_alter_type) {
		auto extra = StringUtil::Lower(
		    Scalar(raw, "SELECT CONCAT(COALESCE(EXTRA,''),'|',COALESCE(GENERATION_EXPRESSION,'')) "
		                "FROM information_schema.COLUMNS WHERE " +
		                    pred + " AND BINARY COLUMN_NAME=BINARY " + MetadataLiteral(canonical_column)));
		if (extra.find("auto_increment") != string::npos || extra.back() != '|') {
			error = "0A000";
			throw IOException("DDL cannot alter identity or generated column type");
		}
	} else {
		auto gen_self = Scalar(raw, "SELECT COALESCE(GENERATION_EXPRESSION,'') FROM information_schema.COLUMNS WHERE " +
		                                pred + " AND BINARY COLUMN_NAME=BINARY " + MetadataLiteral(canonical_column));
		auto extra_self = StringUtil::Lower(
		    Scalar(raw, "SELECT COALESCE(EXTRA,'') FROM information_schema.COLUMNS WHERE " + pred +
		                    " AND BINARY COLUMN_NAME=BINARY " + MetadataLiteral(canonical_column)));
		if (!gen_self.empty() || extra_self.find("auto_increment") != string::npos) {
			error = extra_self.find("auto_increment") != string::npos ? "0A000" : "2BP01";
			throw IOException("DDL cannot drop identity or generated column");
		}
	}
}

static void ValidateLosslessWidening(const DDLColumnSpec &spec, const string &data_type, const string &column_type,
                                     int32_t num_prec, int32_t num_scale, int32_t char_len, int32_t dt_prec,
                                     string &error) {
	const auto lower_dt = StringUtil::Lower(data_type);
	const auto lower_ct = StringUtil::Lower(column_type);
	if (lower_ct.find("unsigned") != string::npos || lower_ct.find("zerofill") != string::npos) {
		error = "0A000";
		throw IOException("DDL cannot alter unsigned or zerofill column");
	}
	bool allowed = false;
	if (lower_dt == "smallint") {
		allowed = spec.type == "integer" || spec.type == "bigint";
	} else if (lower_dt == "int") {
		allowed = spec.type == "bigint";
	} else if (lower_dt == "float" && lower_ct == "float") {
		allowed = spec.type == "double";
	} else if (lower_dt == "decimal") {
		int32_t p = 0, s = 0;
		if (ParseTwoUInts(lower_ct, "decimal", p, s) && num_prec > 0) {
			p = num_prec;
			s = num_scale;
		}
		allowed = spec.type == "numeric" && p >= 1 && spec.precision > p && spec.precision <= 65 && spec.scale == s &&
		          spec.scale <= 30;
	} else if (lower_dt == "varchar") {
		int32_t n = 0, dummy = 0;
		if (char_len > 0) {
			n = char_len;
		} else {
			ParseTwoUInts(lower_ct, "varchar", n, dummy);
		}
		allowed = spec.type == "varchar" && n > 0 && spec.length > n && spec.length <= 16383;
	} else if (lower_dt == "datetime") {
		int32_t p = 0, dummy = 0;
		if (ParseTwoUInts(lower_ct, "datetime", p, dummy, 0)) {
			if (dt_prec >= 0) {
				p = dt_prec;
			}
			allowed = spec.type == "timestamp" && spec.precision > p && spec.precision <= 6;
		}
	} else if (lower_dt == "timestamp") {
		int32_t p = 0, dummy = 0;
		if (ParseTwoUInts(lower_ct, "timestamp", p, dummy, 0)) {
			if (dt_prec >= 0) {
				p = dt_prec;
			}
			allowed = spec.type == "timestamptz" && spec.precision > p && spec.precision <= 6;
		}
	} else if (lower_dt == "time") {
		int32_t p = 0, dummy = 0;
		if (ParseTwoUInts(lower_ct, "time", p, dummy, 0)) {
			if (dt_prec >= 0) {
				p = dt_prec;
			}
			allowed = spec.type == "time" && spec.precision > p && spec.precision <= 6;
		}
	}
	if (!allowed) {
		error = "0A000";
		throw IOException("DDL column type conversion is not an allowed lossless widening");
	}
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
		const bool long_timeout = d.intent.contract_version == 4 || d.intent.action == "alter_column_type" ||
		                          d.intent.action == "set_not_null" || d.intent.action == "drop_not_null" ||
		                          d.intent.action == "add_constraint" || d.intent.action == "drop_constraint" ||
		                          d.intent.action == "truncate";
		deadline = make_uniq<GuardedCRUDDeadline>(ctx, mysql_get_socket(raw), long_timeout ? 120 : 30);
		if (raw->server_status & SERVER_STATUS_IN_TRANS)
			throw IOException("DDL session is not idle");
		if (mysql_reset_connection(raw))
			throw IOException("DDL session reset failed");
		auto version = Scalar(raw, "SELECT VERSION()");
		auto comment = Scalar(raw, "SELECT @@version_comment");
		auto lower_comment = StringUtil::Lower(comment);
		auto lower_version = StringUtil::Lower(version);
		bool distribution =
		    ((comment == "Ubuntu" || comment == "(Ubuntu)" || lower_comment.find("ubuntu") != string::npos) &&
		     (lower_version.find("ubuntu") != string::npos || lower_version.find("ubu") != string::npos)) ||
		    ((comment == "Debian" || comment == "(Debian)" || lower_comment.find("debian") != string::npos) &&
		     (lower_version.find("debian") != string::npos || lower_version.find("deb") != string::npos));
		bool mariadb_identity =
		    lower_comment.find("mariadb") != string::npos || lower_version.find("mariadb") != string::npos;
		bool mysql_identity =
		    !mariadb_identity && (distribution || comment.find("MySQL") != string::npos);
		if ((mariadb ? !mariadb_identity : !mysql_identity) ||
		    Scalar(raw, "SELECT DATABASE()") != d.intent.database) {
			error = "0A000";
			throw IOException("DDL source not certified");
		}
		const bool change_column = d.intent.action == "rename_column" &&
		                           (mariadb ? !VersionAtLeast(version, 10, 5, 0) : !VersionAtLeast(version, 8, 0, 0));
		const bool alter_type = d.intent.action == "alter_column_type";
		const bool alter_nullability = d.intent.action == "set_not_null" || d.intent.action == "drop_not_null";
		const bool modify_column = alter_type || alter_nullability;
		const bool schema_action = d.intent.action == "create_schema" || d.intent.action == "drop_schema";
		if (change_column && !mariadb && !VersionAtLeast(version, 5, 7, 0)) {
			error = "0A000";
			throw IOException("DDL column rename has no equivalent native syntax");
		}
		if (((d.intent.action == "add_constraint" && d.intent.constraint_kind == "check") ||
		     (d.intent.action == "create_table" && d.intent.has_check_constraint)) &&
		    (mariadb ? !VersionAtLeast(version, 10, 2, 1)
		             : (!VersionAtLeast(version, 8, 0, 16) ||
		                Scalar(raw, "SELECT CAST(COUNT(*) AS CHAR) FROM information_schema.COLUMNS WHERE "
		                            "TABLE_SCHEMA='information_schema' AND TABLE_NAME='TABLE_CONSTRAINTS' AND "
		                            "COLUMN_NAME='ENFORCED'") != "1"))) {
			error = "0A000";
			throw IOException("DDL enforced CHECK is unavailable");
		}
		if (Scalar(raw, "SELECT CAST((@@read_only<>0) AS CHAR)") != "0" || SessionReadOnly(raw) != "0") {
			error = "25006";
			throw IOException("DDL source read only");
		}
		const string settings =
		    "SET SESSION "
		    "time_zone='+00:00',lock_wait_timeout=5,innodb_lock_wait_timeout=5,sql_mode='STRICT_ALL_TABLES,NO_ZERO_"
		    "DATE,NO_ZERO_IN_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION,NO_BACKSLASH_ESCAPES'";
		if (mysql_real_query(raw, settings.c_str(), settings.size()) || mysql_set_character_set(raw, "utf8mb4"))
			throw IOException("DDL session settings failed");
		if (ctx.IsInterrupted())
			throw InterruptException();

		// Pre-check InnoDB table scope & IF [NOT] EXISTS no-op conditions BEFORE acquiring table write locks.
		const string scoped_table = schema_action
		                                ? string()
		                                : ((d.intent.action == "rename_index" || d.intent.action == "drop_index")
		                                       ? d.intent.owner
		                                       : d.intent.name);
		if (d.intent.contract_version >= 3 && !scoped_table.empty()) {
			CheckOrdinaryInnoDBTable(raw, d.intent.schema, scoped_table, error);
		}
		if (d.intent.conditional) {
			stage = "source_noop";
			bool noop = false;
			if (d.intent.action == "create_table") {
				noop = TableExists(raw, d.intent.schema, d.intent.name);
			} else if (d.intent.action == "drop_table") {
				noop = !TableExists(raw, d.intent.schema, d.intent.name);
			} else if (d.intent.action == "add_column") {
				if (TableExists(raw, d.intent.schema, d.intent.name) &&
				    ColumnExists(raw, d.intent.schema, d.intent.name, d.intent.column_specs[0].name)) {
					noop = true;
				}
			} else if (d.intent.action == "drop_column") {
				if (!TableExists(raw, d.intent.schema, d.intent.name)) {
					error = "42P01";
					throw IOException("DDL target table does not exist");
				}
				noop = !ColumnExists(raw, d.intent.schema, d.intent.name, d.intent.column_name);
			} else if (d.intent.action == "create_index" || d.intent.action == "create_unique_index") {
				if (TableExists(raw, d.intent.schema, d.intent.name) &&
				    IndexExists(raw, d.intent.schema, d.intent.name, d.intent.index)) {
					noop = true;
				}
			} else if (d.intent.action == "drop_index") {
				if (d.intent.owner.empty()) {
					if (Scalar(raw, "SELECT COUNT(DISTINCT TABLE_NAME) FROM information_schema.STATISTICS WHERE "
					                "TABLE_SCHEMA=" +
					                    MetadataLiteral(d.intent.schema) + " AND LOWER(INDEX_NAME)=LOWER(" +
					                    MetadataLiteral(d.intent.name) + ") AND INDEX_NAME<>'PRIMARY'") != "0") {
						error = "0A000";
						throw IOException("DDL index appeared or changed ownership before execution");
					}
					noop = true;
				} else {
					noop = !TableExists(raw, d.intent.schema, d.intent.owner) ||
					       !IndexExists(raw, d.intent.schema, d.intent.owner, d.intent.name);
				}
			} else if (schema_action) {
				const bool schema_present =
				    Scalar(raw, "SELECT count(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME=" +
				                    MetadataLiteral(d.intent.name)) != "0";
				noop = (d.intent.action == "create_schema" && schema_present) ||
				       (d.intent.action == "drop_schema" && !schema_present);
			}
			if (noop) {
				status = "COMMITTED";
				warnings = 0;
				error.clear();
				stage = "source_noop";
				goto emit_result;
			}
		}

		const auto target =
		    KeywordHelper::WriteQuoted(d.intent.schema, '`') + "." + KeywordHelper::WriteQuoted(d.intent.name, '`');
		if (d.intent.action == "add_column" && !d.intent.column_specs.empty()) {
			const auto &spec = d.intent.column_specs[0];
			if (spec.not_null && spec.default_kind.empty()) {
				if (!TableExists(raw, d.intent.schema, d.intent.name)) {
					error = "42P01";
					throw IOException("DDL target table does not exist");
				}
				if (Scalar(raw, "SELECT COUNT(*) FROM (SELECT 1 FROM " + target + " LIMIT 1) AS fqe_nonempty") != "0") {
					error = "23502";
					throw IOException("DDL cannot add NOT NULL column without DEFAULT to non-empty table");
				}
			}
		}

		if (d.intent.action == "drop_column" || modify_column) {
			if (!TableExists(raw, d.intent.schema, d.intent.name)) {
				error = "42P01";
				throw IOException("DDL target table does not exist");
			}
			if (!ColumnExists(raw, d.intent.schema, d.intent.name, d.intent.column_name)) {
				error = "42703";
				throw IOException("DDL target column does not exist");
			}
		}

		MySQLRenameDefinition definition;
		string canonical_column, default_hex, default_charset, expected_nullable;
		bool string_default = false, nullable_null_default = false;

		if (d.intent.action == "drop_column") {
			stage = "source_definition";
			canonical_column = ResolveColumn(raw, target, d.intent.column_name);
			auto col_count_str = Scalar(raw, "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=" +
			                                     MetadataLiteral(d.intent.schema) +
			                                     " AND TABLE_NAME=" + MetadataLiteral(d.intent.name));
			if (std::stoull(col_count_str) <= 1) {
				error = "42P16";
				throw IOException("DDL cannot drop the last remaining column of a table");
			}
			CheckColumnDependencies(raw, d.intent.schema, d.intent.name, canonical_column, false, error);
		}

		if (change_column || modify_column) {
			stage = "source_lock";
			// 5.7 SHOW CREATE escapes backslashes even with NO_BACKSLASH_ESCAPES
			// set. Match its output convention when executing the retained text.
			ExecuteControl(raw, "SET SESSION time_zone='+00:00',autocommit=0,sql_quote_show_create=1,"
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
		if (d.intent.contract_version >= 3 && !scoped_table.empty()) {
			CheckOrdinaryInnoDBTable(raw, d.intent.schema, scoped_table, error);
		}

		auto native_sql = d.intent.sql;
		if (d.intent.action == "drop_column") {
			native_sql = "ALTER TABLE " + target + " DROP COLUMN " + KeywordHelper::WriteQuoted(canonical_column, '`');
		}
		if (change_column || modify_column) {
			canonical_column = ResolveColumn(raw, target, d.intent.column_name);
			definition = MySQLRenameDefinition::Parse(ShowCreate(raw, target), d.intent.name);
			auto execution_definition = definition;
			const auto properties = Scalar(
			    raw,
			    string("SELECT CONCAT(COALESCE(CHARACTER_SET_NAME,'binary'),':',DATA_TYPE,':',") +
			        (mariadb ? "IF(COLUMN_DEFAULT IS NULL OR COLUMN_DEFAULT='NULL','0','1')"
			                 : "IF(COLUMN_DEFAULT IS NULL,'0','1')") +
			        ",':',IF(COALESCE(GENERATION_EXPRESSION,'')='','0','1'),':',IS_NULLABLE) FROM "
			        "information_schema.COLUMNS WHERE "
			        "TABLE_SCHEMA=" +
			        MetadataLiteral(d.intent.schema) + " AND TABLE_NAME=" + MetadataLiteral(d.intent.name) +
			        " AND BINARY COLUMN_NAME=BINARY " + MetadataLiteral(canonical_column));
			auto parts = StringUtil::Split(properties, ':');
			if (parts.size() != 5) {
				throw std::invalid_argument("DDL source default metadata incomplete");
			}
			nullable_null_default = parts[2] == "0" && parts[3] == "0" && parts[4] == "YES";
			if (parts[3] == "1") {
				if (modify_column) {
					error = "0A000";
					throw IOException("DDL cannot modify generated column");
				}
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
			// neither SHOW nor I_S can distinguish them. Refuse before CHANGE/MODIFY.
			if (parts[0] == "utf8mb4" && (parts[1] == "enum" || parts[1] == "set") &&
			    Scalar(raw, "SELECT LOCATE('?',COLUMN_TYPE) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=" +
			                    MetadataLiteral(d.intent.schema) + " AND TABLE_NAME=" + MetadataLiteral(d.intent.name) +
			                    " AND BINARY COLUMN_NAME=BINARY " + MetadataLiteral(canonical_column)) != "0") {
				throw std::invalid_argument("DDL source enum members cannot be certified");
			}
			if (parts[2] == "1" &&
			    (parts[1] == "char" || parts[1] == "varchar" || parts[1] == "enum" || parts[1] == "set" ||
			     parts[1] == "binary" || parts[1] == "varbinary") &&
			    HasQuotedStringDefault(definition, canonical_column)) {
				string_default = true;
				default_charset = parts[0];
				default_hex = DefaultHex(raw, d.intent.schema, d.intent.name, canonical_column, default_charset);
				execution_definition.StringDefault(canonical_column, parts[0], default_hex);
			}
			if (change_column) {
				native_sql =
				    execution_definition.Change(d.intent.schema, d.intent.name, canonical_column, d.intent.new_name);
			} else if (d.intent.action == "set_not_null") {
				if (parts[4] == "NO") {
					status = "COMMITTED";
					warnings = 0;
					error.clear();
					stage.clear();
					goto emit_result;
				}
				if (Scalar(raw, "SELECT COUNT(*) FROM (SELECT 1 FROM " + target + " WHERE " +
				                    KeywordHelper::WriteQuoted(canonical_column, '`') +
				                    " IS NULL LIMIT 1) AS fqe_null_probe") != "0") {
					error = "23502";
					throw IOException("DDL column contains NULL values");
				}
				execution_definition.SetNullability(canonical_column, true);
				expected_nullable = "NO";
				native_sql = execution_definition.Modify(d.intent.schema, d.intent.name, canonical_column);
			} else if (d.intent.action == "drop_not_null") {
				if (Scalar(raw, "SELECT COUNT(*) FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=" +
				                    MetadataLiteral(d.intent.schema) + " AND TABLE_NAME=" +
				                    MetadataLiteral(d.intent.name) +
				                    " AND INDEX_NAME='PRIMARY' AND BINARY COLUMN_NAME=BINARY " +
				                    MetadataLiteral(canonical_column)) != "0") {
					error = "42P16";
					throw IOException("DDL cannot drop NOT NULL on primary key column");
				}
				auto extra = StringUtil::Lower(
				    Scalar(raw, "SELECT COALESCE(EXTRA,'') FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=" +
				                    MetadataLiteral(d.intent.schema) + " AND TABLE_NAME=" +
				                    MetadataLiteral(d.intent.name) + " AND BINARY COLUMN_NAME=BINARY " +
				                    MetadataLiteral(canonical_column)));
				if (extra.find("auto_increment") != string::npos) {
					error = "0A000";
					throw IOException("DDL cannot drop NOT NULL on AUTO_INCREMENT column");
				}
				if (parts[4] == "YES") {
					status = "COMMITTED";
					warnings = 0;
					error.clear();
					stage.clear();
					goto emit_result;
				}
				execution_definition.SetNullability(canonical_column, false);
				expected_nullable = "YES";
				native_sql = execution_definition.Modify(d.intent.schema, d.intent.name, canonical_column);
			} else if (d.intent.action == "alter_column_type") {
				CheckColumnDependencies(raw, d.intent.schema, d.intent.name, canonical_column, true, error);
				const auto type_info =
				    Scalar(raw, "SELECT CONCAT(DATA_TYPE,'|',COLUMN_TYPE,'|',COALESCE(NUMERIC_PRECISION,-1),'|',"
				                "COALESCE(NUMERIC_SCALE,-1),'|',COALESCE(CHARACTER_MAXIMUM_LENGTH,-1),'|',"
				                "COALESCE(DATETIME_PRECISION,-1)) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=" +
				                    MetadataLiteral(d.intent.schema) + " AND TABLE_NAME=" +
				                    MetadataLiteral(d.intent.name) + " AND BINARY COLUMN_NAME=BINARY " +
				                    MetadataLiteral(canonical_column));
				auto tparts = StringUtil::Split(type_info, '|');
				if (tparts.size() != 6) {
					throw std::invalid_argument("DDL source column type metadata incomplete");
				}
				int32_t num_prec = std::stoi(tparts[2]);
				int32_t num_scale = std::stoi(tparts[3]);
				int32_t char_len = std::stoi(tparts[4]);
				int32_t dt_prec = std::stoi(tparts[5]);
				const auto &spec = d.intent.column_specs[0];
				ValidateLosslessWidening(spec, tparts[0], tparts[1], num_prec, num_scale, char_len, dt_prec, error);
				auto new_native =
				    GuardedDDLIntent::ColumnTypeV5(spec.type, spec.length, spec.precision, spec.scale, true);
				execution_definition.ReplaceType(canonical_column, new_native);
				native_sql = execution_definition.Modify(d.intent.schema, d.intent.name, canonical_column);
			}
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
			const auto name = KeywordHelper::WriteQuoted(d.intent.constraint_name, '`');
			if (kind == "MISSING" && d.intent.conditional) {
				if (Scalar(raw, "SELECT count(*) FROM information_schema.tables WHERE TABLE_SCHEMA=" +
				                    MetadataLiteral(d.intent.schema) + " AND TABLE_NAME=" + MetadataLiteral(d.intent.name)) == "1") {
					status = "COMMITTED";
					warnings = 0;
					error.clear();
					stage.clear();
					goto emit_result;
				}
			} else if (kind == "PRIMARY KEY")
				native_sql = "ALTER TABLE " + target + " DROP PRIMARY KEY";
			else if (kind == "UNIQUE")
				native_sql = "ALTER TABLE " + target + " DROP INDEX " + name;
			else if (kind == "FOREIGN KEY")
				native_sql = "ALTER TABLE " + target + " DROP FOREIGN KEY " + name;
			else if (kind == "CHECK" && (mariadb ? VersionAtLeast(version, 10, 2, 1) : VersionAtLeast(version, 8, 0, 16)))
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
			source.product = "mysql";
			source.state = mysql_sqlstate(raw);
			source.code = std::to_string(mysql_errno(raw));
			source.message = mysql_error(raw);
			auto code = mysql_errno(raw);
			const char *s = mysql_sqlstate(raw);
			error = s && strlen(s) == 5 ? s : "HY000";
			if (code == 3959) {
				error = "2BP01";
			} else if (code == 1138) {
				error = "23502";
			} else if (code == 1205 || code == 3572) {
				error = "55P03";
			} else if (code == 1213) {
				error = "40P01";
			}
			// Unknown/native partial effects are not reported as an unsent request.
			switch (code) {
			case 1007:
			case 1008:
			case 1054:
			case 1064:
			case 1067:
			case 1101:
			case 1171:
			case 1265:
			case 1292:
			case 1364:
			case 1366:
			case 1048:
			case 1062:
			case 1138:
			case 1406:
			case 1832:
			case 1833:
			case 3780:
			case 3819:
			case 3823:
			case 3959:
			case 1050:
			case 1051:
			case 1061:
			case 1060:
			case 1091:
			case 1142:
			case 1146:
			case 1205:
			case 1213:
			case 1217:
			case 1451:
			case 3108: // dependent generated column: rejected before changing the definition
			case 3572:
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
		if ((change_column || modify_column) && !warnings) {
			stage = "source_verify";
			auto after = MySQLRenameDefinition::Parse(ShowCreate(raw, target), d.intent.name);
			if (change_column) {
				if (!definition.Preserved(after, canonical_column, d.intent.new_name, nullable_null_default) ||
				    (string_default &&
				     DefaultHex(raw, d.intent.schema, d.intent.name, d.intent.new_name, default_charset) !=
				         default_hex)) {
					error = "XX000";
					throw IOException("DDL committed column attributes differ from source definition");
				}
			} else {
				if (!definition.PreservedOtherColumns(after, canonical_column) ||
				    (string_default &&
				     DefaultHex(raw, d.intent.schema, d.intent.name, canonical_column, default_charset) !=
				         default_hex)) {
					error = "XX000";
					throw IOException("DDL committed unrelated column attributes changed");
				}
				if (!expected_nullable.empty()) {
					auto after_null = Scalar(
					    raw, "SELECT IS_NULLABLE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=" +
					             MetadataLiteral(d.intent.schema) + " AND TABLE_NAME=" + MetadataLiteral(d.intent.name) +
					             " AND BINARY COLUMN_NAME=BINARY " + MetadataLiteral(canonical_column));
					if (after_null != expected_nullable) {
						error = "XX000";
						throw IOException("DDL committed nullability differs from expected state");
					}
				}
			}
			stage.clear();
		}
	} catch (const std::invalid_argument &) {
		// Parsing/size failures have no source errno; distinguish these from
		// transport failures. A confirmed commit is never rewritten as unsent.
		error = status == "COMMITTED" ? "XX000" : "0A000";
	} catch (...) {
		unsigned int caught_code = 0;
		if (pooled) {
			auto raw = pooled.GetConnection().GetConn();
			caught_code = mysql_errno(raw);
			if (source.message.empty() && caught_code) {
				source.product = "mysql";
				source.state = mysql_sqlstate(raw);
				source.code = std::to_string(caught_code);
				source.message = mysql_error(raw);
			}
		}
		if (error.empty()) {
			if (ctx.IsInterrupted()) {
				error = "57014";
			} else if (caught_code == 1205 || caught_code == 3572) {
				error = "55P03";
			} else if (caught_code == 1213) {
				error = "40P01";
			} else {
				error = !source.state.empty() ? source.state : "08006";
			}
		}
		if (sent && status != "COMMITTED")
			status = "UNKNOWN";
	}
emit_result:
	if (pooled) {
		deadline.reset();
		// Close releases table locks even after failure/cancellation; never
		// return a lock-bearing session to the pool or retry its DDL.
		pooled.Invalidate();
	}
	if (d.diagnostics)
		out.SetValue(5, 0, Value(source.JSON()));
	out.SetValue(0, 0, Value(d.intent.operation));
	out.SetValue(1, 0, Value(status));
	out.SetValue(2, 0, Value(error));
	out.SetValue(3, 0, Value(stage));
	out.SetValue(4, 0, Value::BIGINT(warnings));
	out.SetChildCardinality(1);
}
} // namespace
void RegisterMySQLGuardedDDL(ExtensionLoader &loader) {
  auto register_function = [&](TableFunction function) {
	  function.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		  options.Add("source_diagnostics", LogicalType::BOOLEAN);
		  options.Add("conditional", LogicalType::BOOLEAN);
	  });
	  loader.RegisterFunction(function);
  };
  register_function(TableFunction(
	  "mysql_guarded_ddl",
	  {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY}, Scan,
	  Bind, Init));
}
} // namespace duckdb
