#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "guarded_crud_deadline.hpp"
#include "guarded_ddl_intent.hpp"
#include "storage/mysql_catalog.hpp"

namespace duckdb {
namespace {
struct Data : TableFunctionData {
	MySQLCatalog &catalog;
	GuardedDDLIntent intent;
	Data(MySQLCatalog &c, GuardedDDLIntent d) : catalog(c), intent(std::move(d)) {}
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
	return make_uniq<Data>(db->GetCatalog().Cast<MySQLCatalog>(), std::move(intent));
}
static unique_ptr<FunctionData> Bind(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types, vector<string> &names) {
	return BindContract(ctx, in, types, names, 2);
}
static unique_ptr<FunctionData> BindV3(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types, vector<string> &names) {
	return BindContract(ctx, in, types, names, 3);
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
static void Scan(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
	auto &state = in.global_state->Cast<State>();
	if (state.done)
		return;
	state.done = true;
	auto &d = in.bind_data->Cast<Data>();
	string status = "CONFIRMED_NOT_EXECUTED", error, stage = "source_identity";
	MySQLPooledConnection pooled;
	unique_ptr<GuardedCRUDDeadline> deadline;
	bool sent = false;
	int64_t warnings = 0;
	try {
		if (ctx.IsInterrupted())
			throw InterruptException();
		if (d.catalog.GetBackendCapabilities().kind != MySQLBackendKind::MYSQL_STANDARD) {
			error = "0A000";
			throw IOException("DDL source unsupported");
		}
		pooled = d.catalog.GetConnectionPool().Acquire(dbconnector::pool::AcquireMode::TRY);
		auto raw = pooled.GetConnection().GetConn();
		deadline = make_uniq<GuardedCRUDDeadline>(ctx, mysql_get_socket(raw), 30);
		if (raw->server_status & SERVER_STATUS_IN_TRANS)
			throw IOException("DDL session is not idle");
		if (mysql_reset_connection(raw))
			throw IOException("DDL session reset failed");
		auto version = Scalar(raw, "SELECT VERSION()");
		auto comment = Scalar(raw, "SELECT @@version_comment");
		bool distribution =
			((comment == "Ubuntu" || comment == "(Ubuntu)") && version.find("ubuntu") != string::npos) ||
			((comment == "Debian" || comment == "(Debian)") && version.find("debian") != string::npos);
		if ((version.find("5.7.") != 0 && version.find("8.0.") != 0) ||
			(!distribution && comment.find("MySQL") == string::npos) ||
			Scalar(raw, "SELECT DATABASE()") != d.intent.database) {
			error = "0A000";
			throw IOException("DDL source not certified");
		}
		if (d.intent.action == "rename_column" && version.find("8.0.") != 0) {
			error = "0A000";
			throw IOException("DDL column rename has no equivalent native syntax");
		}
		if (Scalar(raw, "SELECT CAST(@@read_only AS CHAR)") != "0" ||
			Scalar(raw, version.find("5.7.") == 0 ? "SELECT CAST(@@session.tx_read_only AS CHAR)"
												  : "SELECT CAST(@@session.transaction_read_only AS CHAR)") != "0") {
			error = "25006";
			throw IOException("DDL source read only");
		}
		const string settings = "SET SESSION "
								"lock_wait_timeout=5,innodb_lock_wait_timeout=5,sql_mode='STRICT_ALL_TABLES,NO_ZERO_"
								"DATE,NO_ZERO_IN_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION'";
		if (mysql_real_query(raw, settings.c_str(), settings.size()) || mysql_set_character_set(raw, "utf8mb4"))
			throw IOException("DDL session settings failed");
		if (ctx.IsInterrupted())
			throw InterruptException();
		if (d.intent.action == "rename_table" || d.intent.action == "add_column" || d.intent.action == "rename_column" || d.intent.action == "rename_index") {
			auto literal = [&](const string &v) {
				string escaped(v.size() * 2 + 1, '\0');
				escaped.resize(mysql_real_escape_string(raw, &escaped[0], v.c_str(), v.size()));
				return "'" + escaped + "'";
			};
			const string table = d.intent.action == "rename_index" ? d.intent.owner : d.intent.name;
			if (Scalar(raw, "SELECT count(*) FROM information_schema.tables WHERE TABLE_SCHEMA=" + literal(d.intent.schema) +
				" AND TABLE_NAME=" + literal(table) + " AND TABLE_TYPE='BASE TABLE' AND ENGINE='InnoDB'") != "1" ||
			    Scalar(raw, "SELECT count(*) FROM information_schema.partitions WHERE TABLE_SCHEMA=" + literal(d.intent.schema) +
				" AND TABLE_NAME=" + literal(table) + " AND PARTITION_NAME IS NOT NULL") != "0") {
				error = "0A000";
				throw IOException("DDL ordinary InnoDB table scope is unavailable");
			}
		}
		stage = "source_execute";
		sent = true;
		if (mysql_real_query(raw, d.intent.sql.c_str(), d.intent.sql.size())) {
			auto code = mysql_errno(raw);
			const char *s = mysql_sqlstate(raw);
			error = s && strlen(s) == 5 ? s : "HY000";
			// Unknown/native partial effects are not reported as an unsent request.
			switch (code) {
			case 1050:
			case 1051:
			case 1061:
			case 1091:
			case 1142:
			case 1146:
			case 1217:
			case 1451:
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
	} catch (...) {
		if (error.empty())
			error = ctx.IsInterrupted() ? "57014" : "08006";
		if (sent && status != "COMMITTED")
			status = "UNKNOWN";
	}
	if (pooled) {
		deadline.reset();
		pooled.GetConnection().Close();
	}
	out.SetCardinality(1);
	out.SetValue(0, 0, Value(d.intent.operation));
	out.SetValue(1, 0, Value(status));
	out.SetValue(2, 0, Value(error));
	out.SetValue(3, 0, Value(stage));
	out.SetValue(4, 0, Value::BIGINT(warnings));
}
} // namespace
void RegisterMySQLGuardedDDL(ExtensionLoader &loader) {
	loader.RegisterFunction(TableFunction(
		"mysql_guarded_ddl_v2",
		{LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
		Scan, Bind, Init));
	loader.RegisterFunction(TableFunction(
		"mysql_guarded_ddl_v3",
		{LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
		Scan, BindV3, Init));
}
} // namespace duckdb
