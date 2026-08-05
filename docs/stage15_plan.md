# Stage 15：Web 可视化仪表盘与一键演示（P0+P1 已完成，Stage 15 全部完成）

> 状态：**P0 与 P1 均已实现并实机验收（2026-08-05）**。Stage 15 完成。
> 由用户显式请求立项（2026-08-05）。

## 1. 目标

把系统从"curl 可查"升级为"可看、可操作"的可视化控制面，并固化一键演示流程。
**全部复用现有 Control API 与现有配置，零新增运行时依赖，零前端构建**。

面试/演示价值：复用性（换配置即换数据）+ 可观测性（实时看 FPS/延迟/调度状态/事件/关键帧）
+ 安全可逆控制（操作面板直接演示 Stage 11/12 的快照→变更→验证→回滚闭环）。

## 2. 范围（P0/P1 可独立验收）

### P0-1 Control API 增加 CORS 支持（src/control/http_server.cpp）

现状：自写 HTTP/1.1 服务（`http_server.cpp:235` 构造响应头），无 CORS 头；
浏览器跨源 `fetch` 会被拒，`POST application/json` 需要 OPTIONS 预检（当前 405）。

改动（最小、可配置）：
- 配置项 `control.cors: true`（默认关，保持 API 面最小暴露原则）
- OPTIONS 预检响应：`Access-Control-Allow-Origin: *`、`Allow-Methods: GET, POST, OPTIONS`、
  `Allow-Headers: Content-Type`；普通响应追加 `Access-Control-Allow-Origin: *`
- 单测：预检路径 + 响应头存在性

### P0-2 静态仪表盘 web/dashboard.html（纯 HTML/CSS/JS，零依赖）

轮询以下**已存在**端点（实机核实 2026-08-05）：

| 端点 | 面板 |
|---|---|
| GET /streams | 每路状态、FPS、优先级 |
| GET /metrics/summary | P50/P95/P99 延迟分位数 |
| GET /scheduler/state | 调度器状态灯（NORMAL/PRESSURE/THERMAL/CRITICAL/RECOVERY）|
| GET /errors/recent | 最近错误列表 |
| GET /streams/{id} | 单流详情 |
| POST /streams/{id}/infer-interval、/priority、/restart | 操作面板 |
| POST /config/snapshot、/config/rollback | 快照/回滚操作面板（展示 §16 流程与审计）|

页面结构（单文件，无构建）：状态灯区 / 每路卡片（FPS、延迟、温度）/ 事件流区 /
最近错误区 / 操作面板（interval 输入 → POST → 显示审计结果与读回）。轮询间隔 2s，
API 故障时降级显示（与 Stage 11 "API 故障不影响管道"的工程叙事一致）。

页面服务方式：`http_server` 增加 GET /dashboard 返回 web/dashboard.html（几行改动，
保持单进程故事）；备选 `python3 -m http.server` 文档化。

### P1-1 事件流与关键帧上屏（小端点，非事件引擎改动）

事件目前只落 JSONL（API 无事件端点）。增加两个**只读**小端点：
- `GET /events/recent` — 从 events JSONL 读最近 N 条（有界读 ≤64KB，纯读操作）
- `GET /keyframes/{name}` — 从 keyframe_dir 读文件（白名单：仅文件名匹配
  `[a-zA-Z0-9_-]+\.jpg`，拒绝路径穿越；有界大小）

仪表盘展示事件 feed 与最近关键帧缩略图——这是 Demo 2 可视化最直观的部分。
事件引擎/关键帧逻辑本身**零改动**。

### P1-2 一键演示脚本 scripts/demo_run.sh

串联现有能力为一条命令：起 RTSP 服务（复用 `rtsp_serve.sh`）→ 起管道
（`start_pipeline.sh` 模式）→ 提示打开仪表盘 → 引导 Demo 场景步骤
（故障注入/操作面板/回滚）→ `stop` 一键清理。默认用 `configs/streams_openvideo.yaml`
（文件源 42s 自然结束，演示最快路径）；支持 `--stage14` 切到 RTSP 2h 配置。

## 3. 明确不做（边界）

- 不引入 npm / 前端框架 / 构建工具；仪表盘单 HTML 文件
- 不改管道热路径（source/inference/tracker/events/scheduler 核心逻辑零改动）
- 不做事件引擎、Agent、LLM 扩展
- Docker、Grafana、Prometheus /metrics 端点：记录为 P2 后续（需另行显式请求）
- 不新增 C++ 依赖（继续零依赖策略）

## 4. 涉及文件

| 文件 | 动作 |
|---|---|
| src/control/http_server.cpp / control_server.cpp | CORS、GET /dashboard、/events/recent、/keyframes/{name} |
| src/control/control_config.h / config_loader.cpp | control.cors 配置项 |
| tests/test_control_api.cpp（或同类） | CORS/事件端点单测 |
| web/dashboard.html | 新增（纯静态） |
| scripts/demo_run.sh | 新增 |
| docs/stage15_plan.md | 验收后转验收记录 |

## 5. 验收标准（全部 Jetson 实机）

### P0 验收记录（2026-08-05，已通过）

1. **ctest 9/9 全过**；test_control_api **270 checks 0 failures**（新增 CORS 预检/响应头/禁用拒绝、/dashboard 服务与缺失 404 共 13 checks；另修复 http_server 6 处 send_error 不关闭连接的潜伏缺陷——Connection: close 语义下服务器必须总是关闭，否则 read-until-EOF 客户端挂死）
2. **OPTIONS 预检实机**：`curl -X OPTIONS /health` → 200 + ACAO: * + Allow-Methods: GET, POST, OPTIONS + Allow-Headers: Content-Type + Max-Age
3. **跨源请求实机**：带 `Origin: http://example.com` 的 GET /streams → 200 + ACAO: *
4. **GET /dashboard 实机**：11892 字节 text/html（web/dashboard.html）
5. **数据端点实机**（文件源运行中）：/streams 4 路 RUNNING+优先级、/metrics/summary 每路 FPS、/scheduler/state NORMAL cpu 23.6% temp 58.6°C
6. **写操作闭环实机**：interval 1→0 两次 set_infer_interval 全 success、快照 snap_…_9 → rollback success；审计 JSONL 4 条全 success 带 snapshot_id
7. **回归**：检测/事件 JSONL 0 非法、0 ERROR、exit OK

浏览器侧（Windows 主机）：打开 `http://<jetson-ip>:8091/dashboard` 或经 SSH 隧道
`http://127.0.0.1:8091/dashboard` 即可（仪表盘与 API 同源，无需隧道配置 CORS）。

### P1 验收记录（2026-08-05，已通过）

1. **GET /events/recent[?limit=N]**：事件 JSONL 有界尾读（64 KiB 窗口、整行、N 钳位 [1,200] 默认 50）、最新优先、畸形行跳过、文件缺失返回空列表（显示端点永不报错）；单测覆盖 5 项
2. **GET /keyframes**：白名单过滤（`[A-Za-z0-9_-]+\.jpg`）倒序列表、上限 100
3. **GET /keyframes/{name}**：白名单拒绝（evil.png→400、路径穿越→404/400 双路拒绝）、缺失→404、5 MiB 上限、image/jpeg
4. **单测**：test_control_api **294 checks 0 failures**，ctest 9/9
5. **实机（demo_run.sh start，文件源）**：/events/recent 返回真实事件（含 keyframe 文件名）、/keyframes 列表 100 条最新优先、关键帧 GET 200 image/jpeg 67KB（JPEG 1280x720 验证）、退出 exit OK、JSONL 0 非法 0 ERROR
6. **demo_run.sh**：start（打印演示步骤与仪表盘 URL）/ status / stop 全链路实测；stop 幂等清理（管道 SIGINT + MediaMTX/发布进程停止，实测还清掉了此前遗留的 RTSP 栈）
7. **仪表盘新增**：事件流面板（最新 20 条，含 event/class/count/zone 标签）+ 关键帧缩略图条（从事件记录取 keyframe 文件名 + 目录列表兜底，最多 12 张，点击即从 /keyframes 加载）

测试中发现并修复：tail 窗口覆盖整个文件时误丢首行（`read_len < size` 才做首行截断）；
fixture `get()` 未拆 query 导致 `?limit=` 失效（对齐真实 HTTP 层语义）。

## 6. 预计工作量

1–2 个工作日（CORS+端点 ~0.5 天，仪表盘 ~1 天，脚本与验收 ~0.5 天）。

## 7. 立项依据（用户请求记录）

2026-08-05 用户显式请求将"实时 Web 仪表盘 + 一键演示"立项为 Stage 15
（此前在会话中讨论过仪表盘/Prometheus/Demo 脚本/YOLO11n 替换四个候选，选定前者）。
