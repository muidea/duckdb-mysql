#include "duckdb.hpp"
#include "guarded_crud_deadline.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "storage/mysql_catalog.hpp"

namespace duckdb {
namespace {
struct DDLMetadataData : TableFunctionData {
	MySQLCatalog &catalog;
	string schema, name, kind;
	bool describe;
	DDLMetadataData(MySQLCatalog &c, bool d) : catalog(c), describe(d) {
	}
};
struct DDLMetadataState : GlobalTableFunctionState {
	bool done = false;
};
static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<DDLMetadataState>();
}
static unique_ptr<FunctionData> Bind(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types,
                                     vector<string> &names) {
	for (auto &v : in.inputs)
		if (v.IsNull())
			throw BinderException("DDL metadata input is incomplete");
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, in.inputs[0].GetValue<string>());
	if (!db || db->GetCatalog().GetCatalogType() != "mysql")
		throw BinderException("DDL metadata catalog is unavailable");
	auto d = make_uniq<DDLMetadataData>(db->GetCatalog().Cast<MySQLCatalog>(), in.inputs.size() == 4);
	d->schema = in.inputs[1].GetValue<string>();
	if (d->describe) {
		d->kind = in.inputs[2].GetValue<string>();
		d->name = in.inputs[3].GetValue<string>();
	}
	for (auto &s : {d->schema, d->name})
		if (s.size() > 256 || s.find('\0') != string::npos)
			throw BinderException("DDL metadata identifier is invalid");
	if (d->schema.empty() || (d->describe && (d->name.empty() || (d->kind != "table" && d->kind != "index"))))
		throw BinderException("DDL metadata target is invalid");
	names = {"sqlstate", "stage", "document"};
	types = vector<LogicalType>(3, LogicalType::VARCHAR);
	return std::move(d);
}
struct MetadataFailure {
	string state;
};
static string SafeState(unsigned int code) {
	switch (code) {
	case 1044:
	case 1045:
	case 1142:
	case 1143:
	case 1227:
		return "42501";
	case 1193:
	case 1305:
	case 1064:
		return "0A000";
	case 3024:
	case 1317:
		return "57014";
	default:
		return "08006";
	}
}
static string Scalar(MySQLConnection &c, const string &sql, const vector<Value> &params = {}) {
	// Own the statement until its native errno is classified. The generic
	// connection helper destroys it on throw, losing statement-only errno.
	unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)> stmt(mysql_stmt_init(c.GetConn()), mysql_stmt_close);
	if (!stmt)
		throw MetadataFailure {SafeState(mysql_errno(c.GetConn()))};
	auto checked = [&](int rc) {
		if (rc)
			throw MetadataFailure {SafeState(mysql_stmt_errno(stmt.get()))};
	};
	checked(mysql_stmt_prepare(stmt.get(), sql.c_str(), sql.size()));
	if (mysql_stmt_param_count(stmt.get()) != params.size())
		throw MetadataFailure {"0A000"};
	vector<string> values;
	vector<unsigned long> lengths;
	vector<MYSQL_BIND> binds(params.size());
	values.reserve(params.size());
	lengths.reserve(params.size());
	for (auto &p : params) {
		values.push_back(p.ToString());
		lengths.push_back(values.back().size());
	}
	for (idx_t j = 0; j < params.size(); j++) {
		binds[j] = {};
		binds[j].buffer_type = MYSQL_TYPE_STRING;
		binds[j].buffer = const_cast<char *>(values[j].data());
		binds[j].buffer_length = lengths[j];
		binds[j].length = &lengths[j];
	}
	if (!binds.empty())
		checked(mysql_stmt_bind_param(stmt.get(), binds.data()));
	checked(mysql_stmt_execute(stmt.get()));
	checked(mysql_stmt_store_result(stmt.get()));
	if (mysql_stmt_field_count(stmt.get()) != 1 || mysql_stmt_num_rows(stmt.get()) != 1)
		throw MetadataFailure {"54000"};
	vector<char> buffer(1024 * 1024 + 1);
	unsigned long length = 0;
	MYSQL_BIND output = {};
	output.buffer_type = MYSQL_TYPE_STRING;
	output.buffer = buffer.data();
	output.buffer_length = buffer.size();
	output.length = &length;
	checked(mysql_stmt_bind_result(stmt.get(), &output));
	int rc = mysql_stmt_fetch(stmt.get());
	if (rc == MYSQL_DATA_TRUNCATED || length > 1024 * 1024)
		throw MetadataFailure {"54000"};
	checked(rc);
	if (mysql_stmt_warning_count(stmt.get()) != 0)
		throw MetadataFailure {"54000"};
	return string(buffer.data(), length);
}
static const char *CapabilitiesSQL = R"SQL(
SELECT JSON_OBJECT('contract_version',1,'product','mysql','source_version',VERSION(),
 'database',DATABASE(),'database_identity','','server_identity',@@server_uuid,'schema',?,
 'namespace_identity','','namespace_present',EXISTS(SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME=?),
 'source_read_only',(@@read_only<>0 OR ?<>0),
 'session_identity',JSON_OBJECT('user',CURRENT_USER(),'sql_mode',@@session.sql_mode,
  'charset',@@character_set_connection,'collation',@@collation_connection,'lower_case_table_names',@@lower_case_table_names),
 'visibility','PARTIAL','executor_available',FALSE,'identity_protection','UNVERIFIED')
)SQL";
// information_schema is privilege-filtered: absent rows are UNKNOWN, not ABSENT.
// Index matches include every owning table, never choose the first match.
static const char *DescribeSQL = R"SQL(
SELECT JSON_OBJECT('contract_version',1,'product','mysql','source_version',VERSION(),
 'database',DATABASE(),'database_identity','','server_identity',@@server_uuid,'schema',?,'name',?,'kind',?,
 'namespace_identity','','namespace_present',EXISTS(SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME=?),
 'presence','UNKNOWN',
 'object_identity','','metadata_complete',FALSE,'identity_protection','UNVERIFIED',
 'columns',COALESCE((SELECT JSON_ARRAYAGG(JSON_OBJECT('ordinal',ORDINAL_POSITION,'name',COLUMN_NAME,'type',COLUMN_TYPE,
    'nullable',IS_NULLABLE,'default',COLUMN_DEFAULT,'extra',EXTRA,'charset',CHARACTER_SET_NAME,'collation',COLLATION_NAME))
   FROM (SELECT ORDINAL_POSITION,COLUMN_NAME,COLUMN_TYPE,IS_NULLABLE,COLUMN_DEFAULT,EXTRA,CHARACTER_SET_NAME,COLLATION_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=? AND TABLE_NAME=? ORDER BY ORDINAL_POSITION LIMIT 257) ddl_columns),JSON_ARRAY()),
 'indexes',COALESCE((SELECT JSON_ARRAYAGG(JSON_OBJECT('table',TABLE_NAME,'name',INDEX_NAME,'sequence',SEQ_IN_INDEX,
    'column',COLUMN_NAME,'non_unique',NON_UNIQUE,'prefix',SUB_PART,'collation',COLLATION,'method',INDEX_TYPE))
   FROM (SELECT TABLE_NAME,INDEX_NAME,SEQ_IN_INDEX,COLUMN_NAME,NON_UNIQUE,SUB_PART,COLLATION,INDEX_TYPE FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=? AND ((?='table' AND TABLE_NAME=?) OR (?='index' AND INDEX_NAME=?)) ORDER BY TABLE_NAME,INDEX_NAME,SEQ_IN_INDEX LIMIT 257) ddl_indexes),JSON_ARRAY()))
)SQL";
static void Scan(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
	auto &state = in.global_state->Cast<DDLMetadataState>();
	if (state.done)
		return;
	state.done = true;
	auto &d = in.bind_data->Cast<DDLMetadataData>();
	string error, stage = "source_identity", document;
	MySQLPooledConnection pooled;
	unique_ptr<GuardedCRUDDeadline> deadline;
	try {
		if (ctx.IsInterrupted())
			throw InterruptException();
		if (d.catalog.GetBackendCapabilities().kind != MySQLBackendKind::MYSQL_STANDARD) {
			error = "0A000";
			throw IOException("DDL metadata adapter unavailable");
		}
		pooled = d.catalog.GetConnectionPool().Acquire(dbconnector::pool::AcquireMode::TRY);
		auto &c = pooled.GetConnection();
		deadline = make_uniq<GuardedCRUDDeadline>(ctx, mysql_get_socket(c.GetConn()), 5);
		if (c.GetConn()->server_status & SERVER_STATUS_IN_TRANS)
			throw IOException("DDL metadata session is not idle");
		auto version = Scalar(c, "SELECT VERSION()");
		auto comment = Scalar(c, "SELECT @@version_comment");
		bool distribution =
		    ((comment == "Ubuntu" || comment == "(Ubuntu)") && version.find("ubuntu") != string::npos) ||
		    ((comment == "Debian" || comment == "(Debian)") && version.find("debian") != string::npos);
		if ((!distribution && comment.find("MySQL") == string::npos) || Scalar(c, "SELECT DATABASE()") != d.schema) {
			error = "0A000";
			throw IOException("DDL metadata identity mismatch");
		}
		stage = "source_metadata";
		if (d.describe) {
			// Cap the input cardinality before JSON aggregation; no unbounded catalogue dump.
			auto count = Scalar(
			    c,
			    "SELECT CAST(COUNT(*) AS CHAR) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=? AND TABLE_NAME=?",
			    {Value(d.schema), Value(d.name)});
			auto indexes = Scalar(c,
			                      "SELECT CAST(COUNT(*) AS CHAR) FROM information_schema.STATISTICS WHERE "
			                      "TABLE_SCHEMA=? AND ((?='table' AND TABLE_NAME=?) OR (?='index' AND INDEX_NAME=?))",
			                      {Value(d.schema), Value(d.kind), Value(d.name), Value(d.kind), Value(d.name)});
			if (std::stoull(count) > 256 || std::stoull(indexes) > 256) {
				error = "54000";
				throw IOException("DDL metadata bound exceeded");
			}
			document =
			    Scalar(c, DescribeSQL,
			           {Value(d.schema), Value(d.name), Value(d.kind), Value(d.schema), Value(d.schema), Value(d.name),
			            Value(d.schema), Value(d.kind), Value(d.name), Value(d.kind), Value(d.name)});
		} else {
			// tx_read_only was renamed in 8.0; query only the variable supported by
			// this server. Global read_only alone does not describe a read-only session.
			auto session_read_only =
			    Scalar(c, version.find("5.7.") == 0 ? "SELECT CAST(@@session.tx_read_only AS CHAR)"
			                                        : "SELECT CAST(@@session.transaction_read_only AS CHAR)");
			document = Scalar(c, CapabilitiesSQL, {Value(d.schema), Value(d.schema), Value(session_read_only)});
		}
		if (document.size() > 1024 * 1024 || mysql_warning_count(c.GetConn()) != 0) {
			error = "54000";
			throw IOException("DDL metadata encoding is incomplete");
		}
		stage.clear();
	} catch (const MetadataFailure &failure) {
		error = ctx.IsInterrupted() ? "57014" : failure.state;
		document.clear();
	} catch (...) {
		if (error.empty()) {
			unsigned int code = pooled ? mysql_errno(pooled.GetConnection().GetConn()) : 0;
			switch (code) {
			case 1044:
			case 1045:
			case 1142:
			case 1143:
			case 1227:
				error = "42501";
				break;
			case 1193:
			case 1305:
			case 1064:
				error = "0A000";
				break;
			default:
				error = ctx.IsInterrupted() ? "57014" : "08006";
				break;
			}
		}
		document.clear();
	}
	if (pooled) {
		deadline.reset();
		pooled.Invalidate();
	}
	out.SetCardinality(1);
	out.SetValue(0, 0, Value(error));
	out.SetValue(1, 0, Value(stage));
	out.SetValue(2, 0, Value(document));
}
} // namespace
void RegisterMySQLDDLMetadata(ExtensionLoader &loader) {
	loader.RegisterFunction(
	    TableFunction("mysql_ddl_capabilities_v1", vector<LogicalType>(2, LogicalType::VARCHAR), Scan, Bind, Init));
	loader.RegisterFunction(
	    TableFunction("mysql_ddl_describe_v1", vector<LogicalType>(4, LogicalType::VARCHAR), Scan, Bind, Init));
}
} // namespace duckdb
