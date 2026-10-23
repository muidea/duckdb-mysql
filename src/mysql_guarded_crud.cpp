#include "duckdb.hpp"
#include "guarded_crud_deadline.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "storage/mysql_catalog.hpp"

namespace duckdb {
namespace {
// Fixed diagnostics only; never return SQL, credentials or vendor messages.
struct GuardRejection : public std::exception {
    const char *state;
    const char *stage;
    GuardRejection(const char *state_p, const char *stage_p) : state(state_p), stage(stage_p) {}
    const char *what() const noexcept override { return "guarded MySQL operation rejected"; }
};
static void Reject(const char *state = "0A000", const char *stage = "capability_dialect") {
    throw GuardRejection {state, stage};
}
static string Quote(const string &s) {
    string out = "\"";
    for (auto c : s) {
        switch (c) { case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break; case '\r': out += "\\r"; break; case '\t': out += "\\t"; break;
        default: if (static_cast<unsigned char>(c) < 32) Reject(); out += c; }
    }
    return out + "\"";
}
static bool SimpleName(const string &s) {
    if (s.empty() || s.size() > 64) return false;
    for (auto c : s) if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '_') return false;
    return true;
}
struct Data : TableFunctionData {
    MySQLCatalog &catalog;
    string schema, table, expected, version, sql;
    vector<Value> params;
    bool execute;
    Data(MySQLCatalog &c, bool exec) : catalog(c), execute(exec) {}
};
struct State : GlobalTableFunctionState { bool done = false; };
static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) { return make_uniq<State>(); }
static unique_ptr<FunctionData> Bind(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types, vector<string> &names) {
    for (auto &v : in.inputs) if (v.IsNull()) Reject();
    auto db = DatabaseManager::Get(ctx).GetDatabase(ctx,in.inputs[0].GetValue<string>());
    if (!db || db->GetCatalog().GetCatalogType() != "mysql") Reject();
    auto data = make_uniq<Data>(db->GetCatalog().Cast<MySQLCatalog>(),in.inputs.size() == 6);
    data->schema=in.inputs[1].GetValue<string>(); data->table=in.inputs[2].GetValue<string>();
    if (!SimpleName(data->schema) || !SimpleName(data->table)) Reject();
    if (data->execute) {
        data->expected=in.inputs[3].GetValue<string>(); data->version=in.inputs[4].GetValue<string>(); data->sql=in.inputs[5].GetValue<string>();
        auto it=in.named_parameters.find("params"); if (it!=in.named_parameters.end()) data->params=StructValue::GetChildren(it->second);
    }
    names={"status","affected_rows","sqlstate","stage","version","descriptor"};
    types={LogicalType::VARCHAR,LogicalType::BIGINT,LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR};
    return std::move(data);
}
static unique_ptr<MySQLResult> Query(MySQLConnection &c,const string &sql,const vector<Value> &params={}) {
    return c.Query(sql,params,MySQLResultStreaming::FORCE_MATERIALIZATION);
}
static string Scalar(MySQLConnection &c,const string &sql,const vector<Value> &params={}) {
    auto r=Query(c,sql,params); if (!r->Next() || r->IsNull(0)) Reject(); auto v=r->GetString(0); if (r->Next()) Reject(); return v;
}
static void CheckVisibility(MySQLConnection &c,const string &schema) {
    auto grants=Query(c,"SHOW GRANTS FOR CURRENT_USER"); bool visible=false;
    while(grants->Next()) {
        auto grant=grants->GetString(0); auto on=grant.find(" ON "),to=grant.find(" TO ");
        if (on==string::npos || to==string::npos) continue;
        auto object=grant.substr(on+4,to-on-4);
        if (object!="*.*" && object!=MySQLUtils::WriteIdentifier(schema)+".*") continue;
        auto permissions=","+grant.substr(6,on-6)+",";
        visible=visible || permissions.find(",ALL PRIVILEGES,")!=string::npos ||
            permissions.find(",TRIGGER,")!=string::npos || permissions.find(", TRIGGER,")!=string::npos;
    }
    if (!visible) Reject("42501", "capability_permission"); // Missing TRIGGER visibility must never certify an empty trigger list.
}
static string Metadata(MySQLConnection &c,const Data &d, bool modern_innodb) {
    vector<Value> args={Value(d.schema),Value(d.table)};
    auto table=Query(c,"SELECT TABLE_TYPE,ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA=? AND TABLE_NAME=?",args);
    if (!table->Next() || table->GetString(0)!="BASE TABLE" || table->GetString(1)!="InnoDB" || table->Next()) Reject();
    auto id=Scalar(c, string("SELECT CAST(TABLE_ID AS CHAR) FROM information_schema.") +
        (modern_innodb ? "INNODB_TABLES" : "INNODB_SYS_TABLES") + " WHERE NAME=CONCAT(?,'/',?)",args);
    auto features=Scalar(c,"SELECT CAST((SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=? AND EVENT_OBJECT_TABLE=?) + (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS WHERE TABLE_SCHEMA=? AND TABLE_NAME=? AND CONSTRAINT_TYPE NOT IN ('PRIMARY KEY','UNIQUE')) + (SELECT COUNT(*) FROM information_schema.PARTITIONS WHERE TABLE_SCHEMA=? AND TABLE_NAME=? AND PARTITION_NAME IS NOT NULL) AS CHAR)",
        {Value(d.schema),Value(d.table),Value(d.schema),Value(d.table),Value(d.schema),Value(d.table)});
    if (features!="0") Reject();
    auto create=Query(c,"SHOW CREATE TABLE "+MySQLUtils::WriteIdentifier(d.schema)+"."+MySQLUtils::WriteIdentifier(d.table));
    if (!create->Next()) Reject(); auto ddl=create->GetString(1);
    string out="{\"database\":"+Quote(d.schema)+",\"table\":"+Quote(d.table)+",\"table_id\":"+Quote(id)+",\"create_sql\":"+Quote(ddl)+",\"columns\":[";
    auto cols=Query(c,"SELECT COLUMN_NAME,DATA_TYPE,COLUMN_TYPE,IS_NULLABLE,COALESCE(CHARACTER_SET_NAME,''),COALESCE(COLLATION_NAME,''),EXTRA,GENERATION_EXPRESSION,COLUMN_DEFAULT FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=? AND TABLE_NAME=? ORDER BY ORDINAL_POSITION LIMIT 4097",args);
    bool first=true;
    while(cols->Next()) {
        if (!cols->GetString(6).empty() || !cols->GetString(7).empty() || !cols->IsNull(8)) Reject();
        if (!first) out+=","; first=false;
        out+="{\"name\":"+Quote(cols->GetString(0))+",\"data_type\":"+Quote(cols->GetString(1))+",\"column_type\":"+Quote(cols->GetString(2))+",\"nullable\":"+(cols->GetString(3)=="YES"?"true":"false")+",\"charset\":"+Quote(cols->GetString(4))+",\"collation\":"+Quote(cols->GetString(5))+"}";
        if (out.size()>1024*1024) Reject();
    }
    return out+"]}";
}
static string ErrorState(unsigned int code) {
    switch(code) {case 1062:return "23505"; case 1048:case 1364:return "23502"; case 1264:return "22003";
    case 1406:return "22001"; case 1213:return "40P01"; case 1205:return "55P03"; case 1142:case 1143:case 1227:case 1044:case 1045:return "42501";
    case 1146:return "42P01"; case 1054:return "42703";
    case 2002:case 2003:case 2006:case 2013:case 2055:return "08006";
    default:return "HY000";}
}
static void Scan(ClientContext &ctx,TableFunctionInput &in,DataChunk &out) {
    auto &state=in.global_state->Cast<State>(); if (state.done)return; state.done=true;
    auto &d=in.bind_data->Cast<Data>(); string status="CONFIRMED_NOT_EXECUTED",error,stage="source_metadata",version,descriptor;
    int64_t affected=-1; bool sent=false,finished=false;
    MySQLPooledConnection pooled;
    unique_ptr<GuardedCRUDDeadline> deadline;
    try {
        if (ctx.IsInterrupted()) throw InterruptException();
        if (d.catalog.GetBackendCapabilities().kind!=MySQLBackendKind::MYSQL_STANDARD) Reject("0A000", "capability_adapter");
        pooled=d.catalog.GetConnectionPool().Acquire(dbconnector::pool::AcquireMode::TRY);
        auto &c=pooled.GetConnection();
        deadline = make_uniq<GuardedCRUDDeadline>(ctx, mysql_get_socket(c.GetConn()));
        // Reject dirty pooled sessions; never implicitly commit a query transaction.
        if (c.GetConn()->server_status & SERVER_STATUS_IN_TRANS) Reject("HY000", "source_session");
        // Prove source identity before issuing MySQL-specific session controls.
        // Wire-compatible systems can expose VERSION()=8.x while rejecting SET.
        // Such a refusal is an adaptation gap, not a transport failure.
        stage="source_identity";
        version=Scalar(c,"SELECT VERSION()");
        if (mysql_get_server_version(c.GetConn()) < 50700) Reject("0A000", "capability_adapter");
        if (d.execute && version!=d.version) Reject("0A000", "source_descriptor");
        auto comment = Scalar(c,"SELECT @@version_comment");
        // Distribution builds may identify their packager instead of using
        // the word MySQL. Do not admit arbitrary wire-compatible products.
        bool distribution = ((comment == "Ubuntu" || comment == "(Ubuntu)") && version.find("ubuntu") != string::npos) ||
                            ((comment == "Debian" || comment == "(Debian)") && version.find("debian") != string::npos);
        if (Scalar(c,"SELECT DATABASE()")!=d.schema ||
            (!distribution && comment.find("MySQL")==string::npos)) Reject("0A000", "capability_adapter");
        auto modern_innodb = Scalar(c,"SELECT CAST(COUNT(*) AS CHAR) FROM information_schema.TABLES WHERE TABLE_SCHEMA='information_schema' AND TABLE_NAME='INNODB_TABLES'") == "1";
        stage="source_session";
        c.Execute("SET SESSION autocommit=1");
        c.Execute("SET SESSION sql_mode='STRICT_ALL_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION,NO_BACKSLASH_ESCAPES'");
        // Session encoding is shared; text predicates still require their own
        // source-column NO PAD contract in the connector. 5.7 has no 0900 collation.
        auto has_0900 = Scalar(c,"SELECT CAST(COUNT(*) AS CHAR) FROM information_schema.COLLATIONS WHERE COLLATION_NAME='utf8mb4_0900_bin'") == "1";
        c.Execute(has_0900 ? "SET NAMES utf8mb4 COLLATE utf8mb4_0900_bin" : "SET NAMES utf8mb4 COLLATE utf8mb4_bin");
        c.Execute("SET SESSION time_zone='+00:00'");
        c.Execute("SET SESSION lock_wait_timeout=5"); c.Execute("SET SESSION innodb_lock_wait_timeout=5");
        CheckVisibility(c,d.schema);
        c.Execute("SET TRANSACTION ISOLATION LEVEL READ COMMITTED"); c.Execute("START TRANSACTION");
        auto lock=Query(c,"SELECT * FROM "+MySQLUtils::WriteIdentifier(d.schema)+"."+MySQLUtils::WriteIdentifier(d.table)+" LIMIT 0 LOCK IN SHARE MODE");
        stage="source_metadata";
        descriptor=Metadata(c,d,modern_innodb);
        if (d.execute) {
            stage="source_descriptor";
            if (descriptor!=d.expected) {error="0A000";Reject();}
            if (ctx.IsInterrupted()) {error="57014";throw InterruptException();}
            stage="source_execute";
            MySQLStatementPtr prepared(mysql_stmt_init(c.GetConn()),MySQLStatementDelete);
            if (!prepared) Reject();
            if (mysql_stmt_prepare(prepared.get(),d.sql.c_str(),d.sql.size())!=0) {error=ErrorState(mysql_stmt_errno(prepared.get()));Reject();}
            auto statement=make_uniq<MySQLStatement>(d.sql,prepared.release(),vector<MySQLField>());
            if (mysql_stmt_field_count(statement->get())!=0 || mysql_stmt_param_count(statement->get())!=d.params.size()) Reject();
            auto raw_statement=statement->get();
            sent=true;
            try {
                auto result=c.Query(*statement,d.params,MySQLResultStreaming::FORCE_MATERIALIZATION);
                affected=result->AffectedRows();
            } catch (...) {error=ErrorState(mysql_stmt_errno(raw_statement));throw;}
            if (mysql_warning_count(c.GetConn())!=0) {error="01000";Reject();}
            stage="source_commit";status="COMMIT_STATUS_UNKNOWN";finished=true;
            c.Execute("COMMIT"); status="COMMITTED";stage.clear();
        } else {c.Execute("ROLLBACK");finished=true;stage.clear();}
    } catch (const GuardRejection &rejection) {
        if (error.empty()) error = rejection.state;
        if (!sent && stage != "source_descriptor" && stage != "source_execute") stage = rejection.stage;
    } catch (...) {
        if (error.empty()) error=ctx.IsInterrupted()?"57014":
            (pooled ? ErrorState(mysql_errno(pooled.GetConnection().GetConn())) : "08006");
    }
    if (pooled) {
        auto &c=pooled.GetConnection();
        if (!finished) {
            try {c.Execute("ROLLBACK");if(sent)status="CONFIRMED_ROLLED_BACK";}
            catch (...) {if(sent)status="COMMIT_STATUS_UNKNOWN";}
        }
        deadline.reset(); // Stop/join the socket watcher before closing or returning the connection.
        pooled.Invalidate(); // No transport reconnect/retry, no session state reuse.
    }
    out.SetCardinality(1);
    out.SetValue(0,0,Value(status));out.SetValue(1,0,Value::BIGINT(affected));out.SetValue(2,0,Value(error));
    out.SetValue(3,0,Value(stage));out.SetValue(4,0,Value(version));out.SetValue(5,0,Value(descriptor));
}
}
void RegisterMySQLGuardedCRUD(ExtensionLoader &loader) {
    loader.RegisterFunction(TableFunction("mysql_crud_metadata_v1",vector<LogicalType>(3,LogicalType::VARCHAR),Scan,Bind,Init));
    TableFunction exec("mysql_guarded_mutation_v1",vector<LogicalType>(6,LogicalType::VARCHAR),Scan,Bind,Init);
    exec.named_parameters["params"]=LogicalType::ANY; loader.RegisterFunction(exec);
}
}
