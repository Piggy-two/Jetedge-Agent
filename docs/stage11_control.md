# Stage 11 验收报告：安全 Control API、配置快照、验证与回滚

**日期**：2026-08-04　**验收状态**：通过（Jetson 实机）

## 1. 目标与范围

在 Agent 之前，把底层能力封装成安全控制层（CLAUDE.md §16 / implementation_plan §55-61）：

- 白名单 HTTP Control API（只读 + 受限写操作）；
- 每个写操作执行统一流程：**参数校验 → 安全状态检查 → 修改前快照 → 有界修改 → 审计 → 读回验证 → 失败自动回滚**；
- 配置快照（JSON 落盘、有界保留）与回滚（恢复全部受影响的字段）；
- 审计日志（JSONL，含 request_id / 前值 / 后值 / 结果 / snapshot_id）；
- 最近错误环形缓冲（喂给 `get_recent_errors`，为 Agent 阶段铺路）；
- Control API 故障绝不影响实时管道。

**不做**（后续阶段）：Agent 工具执行、`run_benchmark`（推迟到 Agent 阶段）、INT8、事件引擎扩展。

## 2. 交付物

### 2.1 新模块 `include/jetedge/control/` + `src/control/`

| 文件 | 职责 |
|---|---|
| `control_config.h` | `control` YAML 组（默认禁用；loopback、小 body 上限、短读超时） |
| `param_validation` | 纯函数参数校验：interval ∈ [0,max]、priority ∈ {high,normal,low}、流存在性、优先级升载判定 |
| `error_store` | 有界（默认 64 条）最近错误环形缓冲，新错误覆盖最旧 |
| `snapshot_store` | 配置快照 JSON 保存/加载/列表，超过 max_snapshots 自动剪除最旧 |
| `audit_log` | JSONL 追加审计（args/before/after 内嵌为真实 JSON 对象） |
| `http_server` | 自写最小 HTTP/1.1（POSIX socket）：请求头 16KiB 上限、body 上限、读超时、Connection: close、纯函数解析器可单测 |
| `control_backend` | ControlServer 与 Pipeline 的接口（FakeBackend 可单测整个写流程） |
| `control_server` | 路由 + 响应信封 `{success, request_id, timestamp_ms, data|error_code, snapshot_id}` + §16 写流程 |

**自写 HTTP 服务器的原因**：Jetson 上无 cpp-httplib / mongoose，且项目禁止安装系统包。控制 API 低频率，自写实现满足全部需求且零新依赖。

### 2.2 端点

```
GET  /health                      → {"status":"ok","streams":N}
GET  /metrics/summary             → 每流帧数/FPS（含 latest_* 滑窗）
GET  /streams | /streams/<id>     → 每流 type/state/priority/infer_interval/frames/重连计数
GET  /scheduler/config | /state   → 调度器配置与状态（含最近系统采样）
GET  /errors/recent               → 最近错误（环形缓冲）
POST /streams/<id>/infer-interval {"interval":0..5}
POST /streams/<id>/priority      {"priority":"high|normal|low"}
POST /streams/<id>/restart       {}   （仅 RTSP；每流 30s 最小间隔）
POST /config/snapshot            {"reason":"..."}
POST /config/rollback            {"snapshot_id":"..."}
```

### 2.3 Pipeline 控制面（pipeline.h/.cpp 增量）

- `runtime_priorities_`：运行时可改的有效优先级（调度 tier 映射改读它）；
- 所有触碰主循环状态的操作用 `g_main_context_invoke_full` 派发到 GLib 主循环线程（有界 5s 等待、超时优雅失败），控制线程永不直接碰 GStreamer/调度器状态；
- `restart_stream`：仅 RTSP、FAILED 拒绝复活、走既有 `schedule_reconnect("control-restart")` 状态机；
- `record_control_error`：bus ERROR（流级 WARN / 致命 ERROR）与 RTSP FAILED 喂入 error_store；
- `apply_snapshot` 恢复优先级 + 间隔，随后若调度器启用则重同步其表。

### 2.4 配置

`configs/streams_stage11.yaml`：Stage 9 四路 RTSP 配置 + `control` 组（本机 8080 被 Open WebUI 占用 → 使用 **8090**，并注释说明）。

## 3. 单元测试（test_control_api，206 checks 全过）

| 分组 | 覆盖 |
|---|---|
| 参数校验 | interval 边界（0/5/-1/6/类型错）、priority 枚举、流查找、升载判定 |
| 错误缓冲 | 容量裁剪、新错误覆盖最旧、newest-first、limit |
| 快照 | 保存/加载 round-trip、缺失/畸形文件拒绝、>max_snapshots 剪除 |
| 审计 | JSONL 单行记录、字段完整、args 内嵌对象 |
| HTTP 解析 | 请求行（含 percent 解码）、头（小写键）、Content-Length、空行容忍、body 截断/超上限拒绝 |
| 写流程（FakeBackend） | 成功路径（快照落盘 + 审计 + 读回一致）；非法参数全拒绝且零副作用；CRITICAL 门控（升载拒绝/降载放行）；apply 失败自动回滚；读回不符自动回滚；restart 节流；快照/回滚端点；未知快照 404 |

## 4. 实机验收（Jetson Orin Nano 8GB，RTSP 4 路，~25 min）

测试环境：MediaMTX + ffmpeg 推流（cam1-4 运行中）；`./build/jetedge_server configs/streams_stage11.yaml`。

### 4.1 Control API 故障隔离（意外收获）

首启配置端口 8080 被本机 Open WebUI（uvicorn）占用 → `HTTP012 bind failed` + `CTL100` → **管线照常运行**（4 路 RUNNING、零中断）。换 8090 后全部端点可用。这本身就是"Control API 故障不影响管道"的实机证据。

### 4.2 只读端点（全部实测）

```
GET /health          → {"data":{"status":"ok","streams":4}, "success":true, ...}
GET /streams         → cam1 RUNNING high interval=0 / cam2 normal / cam3 normal / cam4 low
GET /streams/cam1    → 含 type=rtsp、frames、reconnect_count、last_reason 等全字段
GET /metrics/summary → 每流 input/infer/output 帧数 + 平均/滑窗 FPS + 检测数（实测 ~22-25 fps/流）
GET /scheduler/state → state=NORMAL table=[0 0 0] cpu=39.1% mem=77.6% temp=64.5°C adjustments=0/2
GET /scheduler/config→ 全部阈值/滞回/预算字段
GET /errors/recent   → count=0（全程零管道错误）
```

### 4.3 写操作校验（非法输入全部拒绝，零副作用）

| 请求 | 结果 |
|---|---|
| `interval: 6` / `-1` / `"2"`(字符串) / 缺字段 | 400 PARAM_INTERVAL |
| body 非 JSON | 400 PARAM_JSON |
| 未知流 `cam9` | 404 PARAM_STREAM |
| 未知路径 / DELETE 方法 | 404 PARAM_PATH / 405 HTTP_METHOD |

### 4.4 合法写操作 + 快照 + 回滚（全部实测）

1. `POST /streams/cam4/infer-interval {"interval":2}` → `success:true`，读回 `infer_interval:2`，返回 `snapshot_id: snap_1785832077505_14`；快照文件记录修改前全配置（4 流 priority+interval）；审计 1 条；
2. **节流实机生效**：interval=2 期间（约 80s）cam4 帧数比同段 cam3 少约 1850 帧（每 3 帧保留 1 帧），回滚后 latest fps 恢复 ~25；
3. `POST /streams/cam2/priority {"priority":"low"}` → success，读回 `priority:low`；
4. `POST /config/snapshot {"reason":"acceptance baseline"}` → 显式快照；
5. `POST /config/rollback {"snapshot_id":<显式快照>}` → success（快照本身在修改后创建，故保持 low —— 语义正确）；
6. `POST /config/rollback {"snapshot_id":snap_..._14}`（最早的修改前快照）→ **4 流全部字段恢复**：cam2 low→normal、cam4 interval 2→0；
7. 未知快照 → 404 PARAM_SNAPSHOT。

### 4.5 restart（RTSP 专用 + 节流）

- `POST /streams/cam1/restart` → success（读回 RECONNECTING）；~12s 后 cam1 自动回到 **RUNNING**（reconnects=1，帧恢复流动）；
- 立即再次 restart → 409 **RESTART_THROTTLED**（30s 最小间隔）；
- 文件流 restart → 409 RTSP_ONLY（单测覆盖）。

### 4.6 调度器交互（文档化语义）

调度器启用时，控制面设置的间隔**在当前状态下持续生效，直到调度器下次状态切换**才被其表值覆盖（实测 cam4 interval=2 在 NORMAL 下持续 ~80s，直到回滚）。CRITICAL 下升载操作被安全门拦截（单测覆盖）。语义：`控制面 = 操作员手动覆盖（有界）→ 确定性状态机 = 策略权威（状态迁移时重新断言）`。

### 4.7 稳定性与退出

- 全程管道日志 **0 条 ERROR**；`/errors/recent` count=0；
- 检测 JSONL 52,112 行 + 事件 JSONL 3,113 行，逐行 JSON 校验 0 失败；
- RSS 634,748 kB ≈ 620 MiB（与 Stage 5-9 一致，无持续增长）；
- SIGINT → `exit OK`，进程退出码 0（后台任务通知确认）。

### 4.8 审计日志（6 条全 success）

```
set_infer_interval cam4 {'interval': 2}   → after.infer_interval=2, snapshot_id=snap_..._14
set_priority       cam2 {'priority':'low'}
config_snapshot    {'reason':'acceptance baseline'}
rollback           {'snapshot_id': snap_1785832107641_19}
restart_stream     cam1
rollback           {'snapshot_id': snap_1785832077505_14}
```

## 5. 缺陷修复记录（本阶段开发过程）

1. **audit args 落盘格式**：初版把 JSON 字符串原样写为字符串，消费端需二次解析 → 改为内嵌真实 JSON 对象（解析失败才退化为字符串）；
2. **测试临时目录残留**：`std::rand()` 目录名确定性 + 未清理 → 跨运行审计文件累积、格式混合导致 jsoncpp `resolveReference` 崩溃 → 每次运行先 `remove_all` + 取最后一行；
3. **测试语义错误**：Content-Length 字节数数错（15 非 14/18）、cam1 已 high 再设 high 不构成升载（安全门测试需先降载）——均为测试修正，非产品代码缺陷；
4. `write()` 返回值告警清理（EPIPE 为正常断连路径）。

## 6. 遗留与后续

- `run_benchmark` 端点未实现（Agent 阶段：独立 benchmark 进程 + 受控测量窗口）；
- CRITICAL 安全门只拦截"升载"写操作；rollback 总是放行（恢复已知良好状态优先于负载规则，已在代码注释说明）；
- 调度器启用时手动间隔的"覆盖至下次状态切换"语义未在 UI/文档外暴露（建议 Agent 阶段在工具说明中体现）；
- 审计/快照文件保留策略：快照自动剪除至 max_snapshots=32；审计 JSONL 为追加型（长稳运行需外部轮转——属最终稳定性验收项）；
- 本机同时运行着上一会话遗留的 Stage 9 管线（未属本次工作，未触碰）；实测 CPU/温度为该负载下数值。

## 7. 验收标准对照（implementation_plan §61）

| 检查项 | 结果 |
|---|---|
| CLI/curl 能查询真实指标 | ✓ 只读端点全过（4.2） |
| 能修改 infer interval | ✓ 读回验证 + 节流实机生效（4.4） |
| 非法参数被拒绝 | ✓ 7 类非法请求全拒绝、零副作用（4.3） |
| 修改前生成快照 | ✓ 每次写操作落盘快照（4.4/4.8） |
| rollback 能恢复原值 | ✓ 全部字段恢复（4.4-6） |
| API 异常不影响 Pipeline | ✓ 8080 被占→降级继续；0 ERROR；退出干净（4.1/4.7） |
| 审计日志含前值、后值和结果 | ✓ before/after/success/error_code/snapshot_id（4.8） |

Git 提交建议：`feat: expose safe control api with snapshot and rollback`
