# DuckDB MySQL Scanner Schema Strategy 设计评审稿

## 1. 背景

fedquery 当前以 MySQL 数据源类型注册 StarRocks。这个外部约束短期内无法打破，因此 `duckdb-mysql` 需要在同一个 `mysql_scanner` 扩展内兼容两类 backend：

- 标准 MySQL / MariaDB：保留现有 prepared statement + information_schema 路径。
- StarRocks 2.5 legacy MySQL protocol：禁止依赖 prepared statement，避免 `information_schema.schemata` 触发 catalog recovery loop。

现场错误链路已经表明，StarRocks 2.5 下会在 DuckDB MySQL extension 的 prepared metadata/recovery 路径失败：

```text
Failed to prepare MySQL query "SELECT schema_name FROM information_schema.schemata"
Unsupported command(COM_STMT_PREPARE)
Explicit transaction only support begin/commit/rollback/insert/update/delete/set/select/show statements
```

## 2. 评审结论

原方案的大方向成立：必须从“单一 MySQL 标准协议假设”改为“backend-aware schema/execution strategy”。

但原方案中“不允许改动 scan execution path”的约束不可成立。原因是当前 `Query()`、`Prepare()`、`mysql_query()` 绑定和结果读取均依赖 `mysql_stmt_prepare`。如果只把 schema SQL 从 `information_schema.columns` 替换成 `SHOW FULL COLUMNS`，但仍通过 `transaction.Query()` 执行，仍会发送 `COM_STMT_PREPARE`。

修订后的约束是：

- 不改 DuckDB 上层 table function 语义。
- 不改 optimizer/pushdown 的 MySQL 标准路径。
- 允许在 connector 内部新增 backend-aware text protocol result reader。
- StarRocks legacy backend 的 detect、metadata、query bind、query execution 均不得调用 `mysql_stmt_prepare`。

## 3. 证据链

### 3.1 当前代码证据

| 结论 | 证据 |
| --- | --- |
| 当前 `Query()` 默认使用 prepared statement | `src/mysql_connection.cpp:140-147` 调用 `QueryInternal(... PREPARED_STATEMENT)` |
| prepared statement 会发送 prepare | `src/mysql_connection.cpp:64-69` 调用 `mysql_stmt_prepare` |
| 当前 basic protocol 只在 `Execute()` 无参数场景使用，且不返回结果 | `src/mysql_connection.cpp:52-59` 使用 `mysql_real_query/mysql_store_result` 后返回 `nullptr` |
| schema 枚举依赖 `information_schema.schemata` | `src/storage/mysql_schema_set.cpp:17-24` |
| table/column 元数据依赖 `information_schema.columns` | `src/storage/mysql_table_set.cpp:52-62`、`src/storage/mysql_table_set.cpp:86-101` |
| `mysql_query()` streaming bind 阶段强制 prepare | `src/mysql_scanner.cpp:698-707` 调用 `conn->Prepare(sql)` |
| `mysql_query()` non-streaming bind 阶段也走 prepared `Query()` | `src/mysql_scanner.cpp:679-687` |

这些证据确认：要支持 StarRocks 2.5，必须新增 text protocol 查询结果读取能力，而不是仅替换元数据 SQL。

### 3.2 外部协议证据

| 结论 | 证据 |
| --- | --- |
| MySQL C API text protocol 合法执行入口是 `mysql_real_query` | MySQL 官方 C API 文档：`mysql_real_query()` executes the SQL statement pointed to by the provided string. https://dev.mysql.com/doc/c-api/8.3/en/mysql-real-query.html |
| MySQL prepared statement 路径需要 `mysql_stmt_prepare` | MySQL 官方 C API prepared statement 文档说明需要 `mysql_stmt_init()` 后调用 `mysql_stmt_prepare()`。https://dev.mysql.com/doc/c-api/8.0/en/c-api-prepared-statement-interface-usage.html |
| StarRocks 使用 MySQL protocol 接入 | StarRocks 官方文档说明可通过 MySQL client/JDBC 连接。https://docs.starrocks.io/docs/sql-reference/System_limit/ |
| StarRocks prepared statements 从 v3.2 开始提供 | StarRocks 官方 prepared statement 文档说明该能力从 v3.2 onwards 提供。https://docs.starrocks.io/docs/sql-reference/sql-statements/prepared_statement/ |

因此，对 StarRocks 2.5 使用 text protocol 作为兼容路径是合法且必要的；对 MySQL 8 标准 backend 保留 prepared statement 路径也是合法且必要的。

## 4. Backend 能力模型

新增 backend capability，而不是只用数据源类型判断。

```cpp
enum class MySQLBackendKind {
    MYSQL_STANDARD,
    MARIADB_STANDARD,
    STARROCKS_LEGACY,
    MYSQL_LIKE_UNKNOWN
};

struct MySQLBackendCapabilities {
    MySQLBackendKind kind;
    string version;
    string version_comment;
    bool supports_prepared_statement;
    bool supports_information_schema_schemata;
    bool supports_text_protocol_result;
};
```

### 4.1 探测原则

探测阶段必须使用不会触发 prepared statement 的方式：

- 优先使用 raw connection 能力：`mysql_get_server_info()`。
- 需要 SQL 时，使用 `mysql_real_query()` 执行 `SELECT VERSION()`、`SELECT CURRENT_VERSION()`、`SELECT @@version_comment`。
- 探测失败时，不应回退到 prepared statement。

### 4.2 判定规则

```text
if version/comment/current_version contains "StarRocks":
    if major_version < 3.2:
        kind = STARROCKS_LEGACY
        supports_prepared_statement = false
        supports_information_schema_schemata = false
    else:
        kind = MYSQL_LIKE_UNKNOWN
        supports_prepared_statement = probe_prepared_safely_or_configured

else if prepared probe succeeds:
    kind = MYSQL_STANDARD 或 MARIADB_STANDARD
    supports_prepared_statement = true

else:
    kind = MYSQL_LIKE_UNKNOWN
    supports_prepared_statement = false
```

说明：StarRocks 3.2+ 是否直接启用 prepared statement 不应只靠版本号推断，建议保留显式配置或一次性能力探测，避免不同小版本/部署参数差异。

## 5. 查询接口分层

### 5.1 标准 prepared result path

保留现有实现：

```text
MySQLConnection::Query()
  -> mysql_stmt_prepare
  -> mysql_stmt_execute
  -> mysql_stmt_result_metadata
  -> mysql_stmt_fetch
```

适用：

- MySQL 8.x
- MariaDB 标准场景
- StarRocks 3.2+ 且能力探测确认支持 prepared statement 的场景

### 5.2 新增 text protocol result path

新增 connector 内部 API：

```cpp
unique_ptr<MySQLTextResult> MySQLConnection::QueryText(const string &sql);
```

内部使用：

```text
mysql_real_query
mysql_store_result 或 mysql_use_result
mysql_fetch_fields
mysql_fetch_row
mysql_fetch_lengths
```

该路径负责：

- 读取字段名和 MySQL protocol field type。
- 将字段转换成 DuckDB `LogicalType`。
- 将 text protocol row 转换成 DuckDB vector。
- 支持 `SHOW`、`SELECT`、`LIMIT 0`、普通表扫描 SQL。

这是本方案有效性的关键条件。没有这个路径，StarRocks 2.5 下无法保证不触发 `COM_STMT_PREPARE`。

## 6. Schema Strategy Router

统一入口基于 capability 决策，而不是硬编码数据源类型。

```cpp
Schema ResolveSchema(ClientContext &context, MySQLConnection &conn, TableRef table) {
    auto caps = conn.GetBackendCapabilities();

    if (caps.supports_prepared_statement && caps.supports_information_schema_schemata) {
        return ResolveStandardSchema(context, conn, table);
    }

    if (caps.kind == MySQLBackendKind::STARROCKS_LEGACY) {
        return ResolveStarRocksLegacySchema(context, conn, table);
    }

    return ResolveTextProtocolFallbackSchema(context, conn, table);
}
```

## 7. Metadata 获取策略

### 7.1 MySQL 标准路径

保持现有逻辑：

```text
information_schema.schemata
information_schema.columns
prepared statement metadata
```

收益：

- 保持 MySQL 8/MariaDB 行为不变。
- 保留现有 predicate/aggregate pushdown 的类型基础。
- 避免为了 StarRocks 兼容性破坏已有业务逻辑。

### 7.2 StarRocks legacy 路径

必须走 text protocol：

```text
SHOW DATABASES
SHOW TABLES FROM `db`
SHOW FULL COLUMNS FROM `db`.`table`
```

列解析字段：

```text
Field
Type
Null
Key
Default
Extra
Comment
```

最小实现要求：

- schema 枚举替代 `information_schema.schemata`。
- table 枚举替代从 `information_schema.columns` 反推表。
- table schema 通过 `SHOW FULL COLUMNS` 构造 `ColumnDefinition`。
- 字段类型统一进入 `MySQLTypes::TypeToLogicalType`，只补充 StarRocks 特有类型映射。

### 7.3 Safe fallback

对未知 MySQL-like backend：

```text
SHOW DATABASES
SHOW TABLES FROM db
SHOW FULL COLUMNS FROM db.table
SELECT * FROM db.table LIMIT 0
```

fallback 必须满足：

- 优先 text protocol。
- 如果 backend 不支持 text protocol result，则明确报错，不再进入 prepared recovery loop。
- 错误信息需要携带 backend fingerprint 和已尝试策略。

## 8. StarRocks 类型映射

在 `MySQLTypes::TypeToLogicalType` 增加 StarRocks 类型归一化：

| StarRocks Type | DuckDB Type | 说明 |
| --- | --- | --- |
| LARGEINT | VARCHAR | 避免超出 DuckDB BIGINT/HUGEINT 转换边界导致扫描失败 |
| DATETIMEV2 | TIMESTAMP | StarRocks 时间类型按 DuckDB timestamp 读取 |
| BOOLEAN | BOOLEAN | 标准布尔 |
| JSON | VARCHAR | 保持与现有 MySQL JSON 处理一致 |
| STRING | VARCHAR | StarRocks 字符串类型 |
| DECIMAL / DECIMALV2 / DECIMAL32 / DECIMAL64 / DECIMAL128 | DECIMAL 或 DOUBLE | precision <= 38 使用 DECIMAL，否则 DOUBLE/VARCHAR |
| ARRAY / MAP / STRUCT | VARCHAR | 第一阶段不展开复杂类型 |

第一阶段不追求 StarRocks 类型完美表达，目标是查询稳定和跨 schema 可用。

## 9. Execution Strategy

### 9.1 MySQL 标准执行

保持当前 prepared execution：

```text
Bind:
  conn->Prepare(sql)
  read prepared metadata

Scan:
  conn->Query(stmt, params)
  mysql_stmt_fetch
```

### 9.2 StarRocks legacy 执行

新增 text execution：

```text
Bind:
  generate SQL
  QueryText(sql with LIMIT 0) 或使用 catalog cached schema
  freeze schema

Scan:
  QueryText(final pushed-down SQL)
  mysql_fetch_row
  cast into DuckDB vectors
```

注意：这属于 connector 内部 execution interface 调整，不改变 DuckDB table function 的外部语义。

### 9.3 参数化查询限制

StarRocks legacy backend 不支持 prepared statement，因此 `mysql_query(..., params := row(...))` 不能原样支持。

第一阶段策略：

- 对 StarRocks legacy + params：返回明确错误，提示该 backend 不支持 prepared parameters。
- 对无 params 的 `mysql_query`：使用 text protocol。
- 对 MySQL 标准 backend：保持现有 params prepared 路径。

后续如需支持参数，可增加安全 literal interpolation，但必须单独设计转义和类型规则。

## 10. Schema Cache

第一阶段采用保守缓存，不实现复杂 DDL detect。

```cpp
struct MySQLSchemaCacheKey {
    MySQLBackendKind backend;
    string backend_fingerprint;
    string catalog;
    string schema;
    string table;
};

struct MySQLSchemaCacheEntry {
    vector<ColumnDefinition> columns;
    timestamp_t created_at;
    timestamp_t expires_at;
    uint64_t schema_hash;
};
```

缓存策略：

- key 包含 backend fingerprint，避免 backend 变化后复用旧 schema。
- TTL 到期刷新。
- attach/re-attach 后刷新。
- 手动 clear cache 后刷新。
- 查询 bind 完成后 schema freeze，单次查询执行期间不再漂移。

不建议第一阶段实现“DDL detect refresh”，因为 StarRocks/MySQL-like backend 未必提供稳定 schema version。

## 11. Recovery Loop 控制

StarRocks legacy backend 一旦识别完成：

- 禁止从 StarRocks query failure 自动进入 prepared metadata recovery。
- 禁止用 `information_schema.schemata` 做 recovery source。
- recovery 只允许走 text metadata refresh。
- 如果 text metadata refresh 失败，直接返回可诊断错误，不循环重试。

错误信息建议包含：

```text
backend=StarRocks
version=...
strategy=text_protocol_metadata
failed_sql=SHOW FULL COLUMNS FROM ...
prepared_disabled=true
```

## 12. 合法性与有效性判定

本方案合法性的依据：

- MySQL C API 明确提供 text protocol 查询入口 `mysql_real_query`。
- MySQL C API prepared statement 与 text protocol 是两套不同入口。
- StarRocks 官方文档说明支持 MySQL protocol 连接，但 prepared statement 从 v3.2 才提供，因此 StarRocks 2.5 不能按 MySQL 8 prepared 能力假设处理。
- 当前扩展已有 basic protocol 执行片段，说明在现有依赖下可以调用 `mysql_real_query`，只是缺少结果集封装。

本方案有效性的依据：

- StarRocks legacy 的 detect、metadata、execution 均通过 text protocol，满足“不发送 `COM_STMT_PREPARE`”。
- MySQL 标准 backend 保留现有路径，降低回归风险。
- capability router 把 StarRocks 兼容逻辑限制在 backend-aware 分支，不引入全局 fork。
- schema freeze + cache key fingerprint 避免 runtime schema drift。

## 13. 实施顺序

1. 增加 backend capability 探测，探测必须使用 raw/basic protocol。
2. 增加 `MySQLTextResult`，封装 `mysql_real_query`、`mysql_store_result`、`mysql_fetch_row`。
3. 增加 text protocol metadata reader：`SHOW DATABASES`、`SHOW TABLES`、`SHOW FULL COLUMNS`。
4. 将 `MySQLSchemaSet` / `MySQLTableSet` 接入 strategy router。
5. 将 `mysql_scan` 的 StarRocks legacy 分支接入 text execution。
6. 将无参数 `mysql_query` 的 StarRocks legacy 分支接入 text execution。
7. StarRocks legacy + params 场景先返回明确错误。
8. 增加 MySQL 标准回归测试、StarRocks legacy text protocol 单元/集成测试。

## 14. 第一阶段验收标准

- MySQL 8/MariaDB 原有测试通过。
- StarRocks 2.5 attach 后不再执行 `information_schema.schemata`。
- StarRocks 2.5 attach/query 日志中不出现 `COM_STMT_PREPARE`。
- `SELECT * FROM fqe_mes_wh.mes_wh.raw_mes_batch LIMIT 1` 可通过 DuckDB 执行。
- StarRocks 多 schema 查询可通过 DuckDB catalog/table function 路径执行。
- `mysql_query('catalog', 'select ...')` 无参数查询可用。
- `mysql_query(..., params := row(...))` 在 StarRocks legacy 下返回明确不支持错误。

## 15. 最终结论

刷新后的方案可行，但前提是必须补齐 text protocol result reader。仅替换 schema SQL 或仅调整 `information_schema` 查询不充分。

推荐按“双路径、能力驱动、最小侵入”的方式实施：

- 标准 MySQL 路径不动。
- StarRocks legacy 路径全程 text protocol。
- connector 内部新增 execution interface，但保持 DuckDB table function 外部行为稳定。
