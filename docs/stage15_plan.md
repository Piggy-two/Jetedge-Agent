# Stage 15 计划：Web 可视化仪表盘与一键演示（planned，未实施）

> 状态：**planned（2026-08-05 由用户显式请求立项）**。本文是计划，不是验收报告；
> 完成标志是全部验收检查在 Jetson 实机通过并更新本文为验收记录。

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

1. ctest 新增用例 PASS（CORS 预检/响应头、/events/recent 有界读、/keyframes 白名单与路径穿越拒绝）
2. Windows 主机浏览器跨源访问 Jetson 仪表盘：4 路数据实时刷新，无 CORS 报错
3. 操作面板实机演示：interval 变更 → 服务端审计出现记录 → 回滚 → 读回一致（沿 Stage 11 验收口径）
4. `demo_run.sh` 一条命令完成启动→演示→清理，全程无 ERROR、`exit OK`
5. 回归：检测/事件 JSONL 0 非法；RSS 收敛；EOS/Ctrl-C 干净
6. 文档更新：README/PROGRESS 状态同步，本文转验收记录

## 6. 预计工作量

1–2 个工作日（CORS+端点 ~0.5 天，仪表盘 ~1 天，脚本与验收 ~0.5 天）。

## 7. 立项依据（用户请求记录）

2026-08-05 用户显式请求将"实时 Web 仪表盘 + 一键演示"立项为 Stage 15
（此前在会话中讨论过仪表盘/Prometheus/Demo 脚本/YOLO11n 替换四个候选，选定前者）。
