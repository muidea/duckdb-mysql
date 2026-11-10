# 仓库协作规范


## 数据库错误诊断基础规约

本扩展的错误处理须遵守 [federated-query-engine/AGENTS.md](../federated-query-engine/AGENTS.md) 中“数据库错误诊断基础规约”。公共结构化传输在主仓 `internal/sourceerrors/native/` 维护，使用 `scripts/sync_query_source_error.py` 同步/校验，禁止另建产品专用协议。必须区分真实源端诊断、驱动诊断缺失、适配器结果解码失败和本地能力拒绝，保留实际阶段和对象/类型上下文，不得猜测 SQLSTATE 或覆盖 DML/DDL 执行状态。该规范不扩大现有能力；原生修改与编译使用 `fedqueryBuilder`，禁止修改 vendor。
