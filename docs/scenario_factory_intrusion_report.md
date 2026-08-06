# 场景测试报告 — 工厂危险区域人员闯入监控

> 状态：2026-08-06 实机验收通过（Jetson Orin Nano 8GB，全部检查项 PASS）。本报告所有数字均来自实机运行，无估算值。
> 分支：`scenario/factory-intrusion`（基座 `621ab81`）
> 运行：2026-08-06 18:01:13 → 18:17:17（RUNNING 窗口 963 s = 16.05 分钟）
> 复现：`bash scripts/run_scenario_factory_intrusion.sh run`

## 1. 场景背景和业务价值

工厂生产区域存在人员闯入禁区（危险设备区、仓储区、人员通道）的安全风险。传统方案需要实时逐帧人工盯屏，或依赖纯云端分析（带宽、延迟、隐私不可控）。本场景把 JetEdge-Agent 已验收的 14 个阶段能力组装成一个完整业务闭环：

```text
边缘实时检测（本地，毫秒级）
  → 连续追踪（track_id，跨帧身份保持）
  → 禁区规则事件 zone_entry（确定性去重：同一 track 同一区域只报一次）
  → 关键帧本地落盘 + 异步 Qwen 视觉复核（不阻塞实时链路）
  → 确定性 Decision Router 分级（confirmed_alert / manual_review / archived / local_rule_only）
  → RTSP 故障隔离（单路断流不影响其余三路，指数退避自动恢复）
  → 安全 Agent（只经白名单工具做可验证、可回滚的优化）
```

业务价值：本地实时告警先于云端复核（不依赖网络）；云端只做低频语义复核（成本可控）；告警分级确定性、可审计；任何环节故障（网络/云端/单路视频源）都不会让实时管道停摆。

## 2. 系统架构图

```text
┌─────────────┐   ffmpeg loop (H.264, TCP)   ┌──────────────────────────────┐
│ 4× Intel 视频 │ ───────────────────────────▶ │ MediaMTX (127.0.0.1:8554)   │
│ (432p)       │                              │  cam1..cam4                  │
└─────────────┘                              └──────────────┬───────────────┘
                                                            │ RTSP
┌────────────────────────────────────────────────────────────▼──────────────────┐
│ build/jetedge_server  (单进程: GStreamer/DeepStream 7.1 管道 + 确定性控制面)      │
│  rtspsrc → 硬件解码(每流钉核) → nvstreammux(1280×720, batch 4) → nvinfer        │
│  (YOLO11s FP16 384×640) → nvtracker(NvDCF) → 事件/元数据探针 → fakesink         │
│  │                                                                             │
│  ├─ 事件引擎: appearance/disappearance/zone_entry（每 track 每 zone 去重）       │
│  │    + 关键帧 (nvds_obj_enc GPU JPEG)                                          │
│  ├─ LlmRouter: 有界优先级队列 → Qwen 异步视觉复核（熔断/超时/重试/丢弃均审计）    │
│  ├─ Decision Router（本场景新增）: 确定性分级 → incidents.jsonl                 │
│  ├─ 调度器: NORMAL/PRESSURE/THERMAL/CRITICAL/RECOVERY（按优先级阶梯降推理率）    │
│  └─ Control API + Web Dashboard (0.0.0.0:8091, CORS)                           │
└─────────────────────────────────────────────────────────────────────────────────┘
        │ POST /benchmark /config/snapshot /streams/<id>/infer-interval ...
┌───────▼─────────────────────────────────────┐
│ Python 运行时 Agent (DeepSeek 候选计划 →     │
│ 快照 → 执行 → 基准窗口 → 验证 → 保留/回滚)   │
└─────────────────────────────────────────────┘
```

本场景唯一代码改动：**Decision Router**（C++ 确定性决策模块，`src/llm/decision_router.cpp` + `include/jetedge/llm/decision_router.h`）——Qwen 复核结果之后的分级逻辑，此前不存在。

## 3. 输入视频

来源：intel-iot-devkit/sample-videos（CC-BY 4.0，2026-08-05 下载，`/home/seeed/jetedge-openvideos/`，仓库外）。经 `scripts/rtsp_serve_factory.sh` 以 ffmpeg 循环发布（`-re -stream_loop -1 -c copy`，H.264 Annex-B，TCP，无 EOS——满足长时运行）。

| 流 | 语义 | 优先级 | 视频 | 分辨率 | 帧率 | 时长 | SHA256 |
|---|---|---|---|---|---|---|---|
| cam1 | 工厂入口 | high | person-bicycle-car-detection.mp4 | 768×432 | 12 fps | 53.9 s | `452b11b7…43140ef` |
| cam2 | 停车区域 | normal | car-detection.mp4 | 768×432 | 12.5 fps | 30.2 s | `d31e0ebf…4cca34` |
| cam3 | 生产区人员通道 + restricted_zone | normal | one-by-one-person-detection.mp4 | 768×432 | 10 fps | 139.4 s | `a5964aa2…9ac07c` |
| cam4 | 普通走廊（故障注入目标） | low | people-detection.mp4 | 768×432 | 12 fps | 49.7 s | `18ffe867…2f92f693` |

cam3 选择依据：单人依次行走、单条轨迹长（实测最长 797 帧），最利于 track_id 连续性取证；禁区区 rect `[560,250,500,200]`（mux 1280×720 空间）由实跑检测数据定稿，person 中心覆盖率 95.7%（4635/4842 帧）。

环境：Jetson Orin Nano 8GB（Dev Kit Super，JetPack 6.2.1，L4T R36.4.3，CUDA 12.6，TensorRT 10.3，DeepStream 7.1.0，GStreamer 1.20.3）；模型 YOLO11s FP16 batch4 384×640（Engine 21.25 MiB，SHA256 `136bd5fd…b06818d`，与 Stage 5 验收一致）；解码线程每流钉核（Stage 10）。

## 4. 完整启动和使用流程

```bash
# 0. 环境预检（分支/二进制/视频/端口/密钥/发布冲突，全部自动检查）
bash scripts/run_scenario_factory_intrusion.sh preflight

# 1. 一键完整运行（≈25 分钟，含 ≥15 分钟浸泡 + 全量校验）
bash scripts/run_scenario_factory_intrusion.sh run 2>&1 | tee logs/scenario_factory_run.log

# 2. 分阶段执行（演示 / 单独调试）
bash scripts/run_scenario_factory_intrusion.sh publish      # MediaMTX + 4 路发布
bash scripts/run_scenario_factory_intrusion.sh start        # 启动 jetedge_server + 解码线程钉核
bash scripts/run_scenario_factory_intrusion.sh wait-ready   # 等待 4 路 RUNNING
bash scripts/run_scenario_factory_intrusion.sh baseline     # /streams + /metrics/summary + /scheduler/state + 60s 基准
bash scripts/run_scenario_factory_intrusion.sh fault        # cam4 断流注入（16s 窗口）与自动恢复
bash scripts/run_scenario_factory_intrusion.sh stop         # SIGINT 优雅退出 + 清理发布

# 3. 浏览器仪表盘（0.0.0.0:8091，仅限可信内网；控制面写操作同端口暴露，配置中有安全提示）
#    http://<jetson-ip>:8091/dashboard
#    面板: 调度器状态灯 / 每路 FPS·优先级·推理间隔·重连计数 / P50/P95/P99 延迟条 /
#          事件流 / 关键帧缩略图 / 快照回滚面板
```

## 5. 人员 track_id 连续追踪证据（cam3）

数据源：`logs/scenario_factory_detections.jsonl`（31,051 行，全部合法 JSON）。

- cam3 person 检测 4,842 帧，129 条不同 track_id；
- **长轨迹 63 条（≥50 帧）**，前 5 长：`t99: 797帧`、`t263: 797帧`、`t379: 686帧`、`t533: 550帧`、`t700: 550帧`（10 fps 源 → 单条轨迹持续 55~80 秒，跨视频循环边界仍保持同一 track_id）；
- 相邻帧 bbox 中心最大位移 107.3 px（10 fps 源 + 1280 宽画面，快速行走的真实跨帧位移；阈值 150 px 仍能捕获任何跨对象 ID 跳变）；
- **track_id 连续性直接证据**：42 条 zone_entry 事件的 track_id 均可在 detections 中找到 ≥50 帧的完整轨迹，zone_entry 触发帧的 bbox 与检测轨迹坐标一致（同一 track 进入禁区即身份保持）。

## 6. zone_entry 事件和关键帧证据

配置：`events.zones: [{name: restricted_zone, stream_id: cam3, rect: [560,250,500,200]}]`，`classes: [0]`（person）。

- 本运行 **42 条 zone_entry，42 条不同 track_id，0 重复**（去重状态机：同一 (stream, track, zone) 只触发一次，track 消失后重臂）；
- 事件行示例（`logs/scenario_factory_events.jsonl`）：

```json
{"ts_ms":1786010965879,"stream_id":"cam3","frame_num":1227,"event":"zone_entry",
 "class_id":0,"class":"person","track_id":7,"confidence":0.6392,
 "bbox":[608.9,290.7,76.8,139.4],"count":null,"zone":"restricted_zone",
 "keyframe":"cam3_t1786010965879_zone_entry.jpg"}
```

- 关键帧：`logs/keyframes_scenario_factory/` 共 149 张（全局上限 150，Stage 14 验收值）；其中 zone_entry 关键帧 7 张（`cam3_t*.jpg`，GPU JPEG 编码，60 KB 级）；关键帧上限耗尽后的事件以元数据-only 降级复核（见 §7，模型诚实拒绝不幻觉——Stage 14 已验证行为）；
- 关键帧与事件一一对应（`keyframe` 字段文件名 → 磁盘文件存在，Dashboard `/keyframes` 可预览）。

## 7. Qwen 复核结果及耗时

配置：`llm.routing.zone_entry_to_qwen: true`，1 工作线程，队列上限 32，超时 30 s×2 重试，熔断 5/30/2；关键帧作为 base64 图像随请求发送（`qwen3.6-flash`，thinking 禁用）。

- **42 次调用全部成功（42/42），0 失败**；
- 每次调用的完整记录在 `logs/scenario_factory_analysis.jsonl`：`orig_ts_ms`（本地事件时间）、`ts_ms`（响应落盘时间）、`latency_ms`（HTTP 调用时长）、`result`（schema 校验后的 JSON：`{confirmed, summary, confidence}`）、`error`；
- 本地事件 → 请求入队：**中位 1 ms**（本地事件即刻入队，不等待云端）；端到端（本地事件 → 响应落盘）：中位 12.8 s，p90 36.5 s，最大 67.9 s（含队列排队与 HTTP 往返）；
- 带关键帧复核（前 7 条，6 条成功）：模型明确基于图像确认——"A person with long hair wearing a gray sweater is clearly visible…"（confirmed=true, high）；
- 关键帧上限后元数据-only 复核（32 条）：**13 次诚实拒绝**（"Visual verification is impossible without a reference image"，confirmed=false，零幻觉）+ **19 次基于检测元数据评估**（"high inference confidence and valid bounding box coordinates"）——两种行为都真实记录了模型输出，无任何改写；
- **Qwen 调用期间四路 FPS 无下降**（§11 实测表：max -0.7% / +1.4%）。

## 8. 告警决策结果（Decision Router，本场景新增组件）

规则表（用户原始规则"high 且 confidence≥0.8"适配到真实 schema `{confirmed, confidence: high|medium|low}`——Qwen 无数值字段，映射偏差已记录）：

| confirmed | confidence | decision | 本运行计数 |
|---|---|---|---|
| true | high | **confirmed_alert** | 15 |
| any | medium | **manual_review** | 15 |
| any | low | **archived** | 12 |
| — | Qwen 失败/超时/Schema 非法 | **local_rule_only** | 0（42/42 成功） |
| — | 未提交（熔断/队列丢弃/未路由） | **local_rule_only** | 0 |
| false | high | archived（not_confirmed_high） | 0（本运行） |
| any | 缺失/未知 confidence | manual_review（fail-safe） | 0（本运行） |

- incidents 42 行全部合法 JSON（`logs/scenario_factory_incidents.jsonl`），每行 9 个必需字段 + `zone/bbox/local_confidence/reason` 附加字段；
- **与事件 1:1**：42 条 zone_entry ↔ 42 条 incidents，0 缺失 0 多余；
- 示例行：

```json
{"event_id":"evt_1786010965879_cam3_7_restricted_zone","stream_id":"cam3","track_id":7,
 "local_event":"zone_entry","qwen_description":"A person is clearly visible...",
 "qwen_risk":"high","qwen_confidence":"high","decision":"confirmed_alert",
 "reason":"confirmed_high","timestamp":1786010978184, ...}
```

- 单元测试 `tests/test_decision_router.cpp`：1483 检查 0 失败（parse 边界/规则表 9 格/写行完整性/并发 100 行/失败与未提交各原因降级）；
- 展示：分级结果写入 incidents.jsonl（日志展示；Dashboard 事件流可见），**未接入短信/企业微信/硬件控制**（需求约束）。

## 9. cam4 断流和恢复时间线

故障注入：运行中停止 cam4 发布 16 秒（保持 RECONNECTING，避免进入终态 FAILED——FAILED 为 §15 防重连风暴的设计行为，连续失败 >5 次后停止自动重连，Control API 亦拒绝 restart）。

| 时刻 (18:0x) | 事件 | 证据 |
|---|---|---|
| 07:20 | `cam-stop cam4`（发布停止） | 驱动日志 |
| 07:28 | watchdog 5s 无帧 → **DEGRADED** → `failure='stall' failures=1 backoff=1000ms → RECONNECTING` | 服务端日志 |
| 07:29 | 重建失败 `gst-error failures=2 backoff=2000ms` | 服务端日志 |
| 07:32 | `failures=3 backoff=4000ms` | 服务端日志 |
| 07:36 | `failures=4 backoff=8000ms` | 服务端日志 |
| 07:40 | `cam-start cam4`（发布恢复） | 驱动日志 |
| 08:00 | **verified: 11.6 fps ≥ 1.0 → RUNNING**（自动恢复，20 s） | 服务端日志 |

- 时间线采样（`logs/scenario_factory_fault_timeline.jsonl`，2 s 间隔）：cam4 `RUNNING→RECONNECTING(rc1→4)`，恢复后 `fault_after.json`：**RUNNING, rc=4**；
- **隔离验证：故障期间 cam1/cam2/cam3 全程 RUNNING，0 新增重连 0 失败**（46→8 采样点全过）；
- 快照：`logs/scenario_factory_fault/fault_{before,mid,after}.json`。

## 10. Agent Benchmark、执行方案、验证与回滚结果

命令：`python3 agent/main.py --goal "保证 cam1 推理 FPS 不低于 10，降低全局 P95 延迟" --config agent/config_scenario_factory.yaml --base-url http://127.0.0.1:8091 --benchmark-duration 30`

> cam1 源 12 fps，FPS 保底取 10（默认 15 在此源上必然不可达）；**未降低任何验收阈值**（p95_trivial 20 ms / 相对 15% / 绝对 8 ms / 容差 5% 均为验收默认值）。

| 环节 | 实测 |
|---|---|
| 修改前 Benchmark | 全局 P95 = **49.2 ms**，全局 FPS = 45.1，drop = 0.15% |
| DeepSeek 候选计划 | 2 条：`set_infer_interval(cam4, 1)`、`set_infer_interval(cam3, 1)`（plan 来源 llm） |
| 白名单及参数校验 | 两工具均在 7 工具白名单内；interval=1 ∈ [0, 5]，校验通过 |
| 配置快照 | `snap_1786010779030_76`（`POST /config/snapshot`，reason=agent_baseline） |
| 实际执行 | 18:06:19 起：`set_infer_interval cam4=1` + `set_infer_interval cam3=1`（每步 scheduler 安全复核通过，未触发 CRITICAL 门控） |
| 修改后 Benchmark | 全局 P95 = **32.9 ms** |
| validator 判定 | `before 49.2 → after 32.9`：相对 -33%、绝对 -16.3 ms ≥ 双阈值；cam1 FPS 保底 11.5 ≥ 9.5（10×0.95）→ **通过** |
| 最终结果 | **保留配置（达标）**，退出码 0；无回滚（`logs/scenario_factory_agent/audit.jsonl` 10 行全 phase、服务端审计 7 行 0 非法：2 benchmark + 1 snapshot + 2 interval + …） |

备注：Agent 结果不预设成功——另一轮运行同样实测达标（48.4→32.9 / 48.9→33.0），若未达标将自动回滚并记录为安全闭环成功（Demo 4 先例）。

## 11. FPS、P95、CPU、内存、温度对比表

基准窗口对比（Qwen 空闲 60 s vs Qwen 活跃 60 s；`logs/scenario_factory_baseline/bench_before.json` ↔ `logs/scenario_factory_bench_during.json`）：

| 流 | input FPS 前后 | lat P50 前后 (ms) | lat P95 前后 (ms) | lat P99 前后 (ms) |
|---|---|---|---|---|
| cam1 | 11.58 → 11.52 (-0.6%) | 22.7 → 30.8 | 34.0 → 47.6 | 45.1 → 54.2 |
| cam2 | 12.32 → 12.48 (+1.4%) | 22.9 → 23.5 | 34.0 → 47.3 | 44.7 → 54.1 |
| cam3 | 10.00 → 10.02 (+0.2%) | 23.5 → 25.3 | 35.1 → 47.8 | 45.1 → 54.2 |
| cam4 | 11.60 → 11.52 (-0.7%) | 22.7 → 31.2 | 33.9 → 47.6 | 45.1 → 54.3 |
| 全局 | 45.5 → 45.5 | 22.8 → 30.3 | 34.2 → 47.6 | 45.1 → 54.2 |

→ **Qwen 调用期间四路 FPS 无下降（≤1.4% 波动）**；延迟窗口间波动 34→48 ms，与 10~12 fps 低帧率源的 mux 批填充相位漂移一致（stability CSV 全程可见渐变，非 Qwen 开关阶梯），已如实记录。

稳定性采样（`logs/scenario_factory_stability.csv`，81 个采样点 × 10 s，覆盖 875 s）：

| 指标 | 实测 |
|---|---|
| RSS | 594.0 → 599.7 MiB（+5.71 MiB，**+0.96%**；窗口对比 max 尾/头 = 1.006） |
| 温度 | 57.0 ~ 58.3 °C（scheduler 阈值 75 °C 之上远未达到） |
| CPU（进程） | 平均 4.1%，最大 4.6% |
| 调度器状态 | NORMAL 全程（76 采样；5 个 NA 为基准窗口服务端单飞阻塞） |
| 检测量 | 31,051 行（cam3 11,938 / cam1 6,529 / cam2 3,096 / cam4 9,488） |

## 12. 所有日志和截图路径

| 产物 | 路径 |
|---|---|
| 运行转录 | `logs/scenario_factory_run.log` |
| 服务端日志 | `logs/scenario_factory_server.log`（DEGRADED/RECONNECTING 退避/verified 证据） |
| 检测 JSONL | `logs/scenario_factory_detections.jsonl`（31,051 行 0 非法） |
| 事件 JSONL | `logs/scenario_factory_events.jsonl`（968 行 0 非法） |
| 分析 JSONL | `logs/scenario_factory_analysis.jsonl`（42 行 0 非法） |
| **incidents JSONL** | `logs/scenario_factory_incidents.jsonl`（42 行 0 非法） |
| 关键帧 | `logs/keyframes_scenario_factory/`（149 张） |
| 基准快照 | `logs/scenario_factory_baseline/{streams,metrics/summary,scheduler/state,bench_before}.json` |
| Qwen 期间基准 | `logs/scenario_factory_bench_during.json` |
| 故障快照 | `logs/scenario_factory_fault/{fault_before,fault_mid,fault_after}.json` + `fault_timeline.jsonl` |
| 稳定性采样 | `logs/scenario_factory_stability.csv` |
| 控制审计 | `logs/control_scenario_factory/audit.jsonl`（7 行 0 非法） |
| Agent 审计/报告 | `logs/scenario_factory_agent/audit.jsonl`（10 行 0 非法）+ `reports/run_*.md` + `run.log` |
| Dashboard | `http://<jetson-ip>:8091/dashboard`（截图可在浏览器侧保存；本报告未附带截图文件） |

## 13. 已知限制

- **Qwen schema 无数值置信度**：原始规则 `confidence>=0.8` 映射为字符串分级（§8 规则表）；`confidence` 字段在 schema 校验中可选，缺失/未知一律升级 manual_review（fail-safe）；`qwen_risk` 为派生字段。
- **关键帧全局上限 150**（Stage 14 验收值）：42 条 zone_entry 中仅前 7 条携带关键帧送审，其后 Qwen 元数据-only 复核（诚实拒绝 13 次 + 元数据评估 19 次，§7）；全程带图需提高上限（资源权衡）。
- **FAILED 为终态**：连续失败 >5 次后停止自动重连（防重连风暴，§15 设计），Control API 亦拒绝 restart；本场景故障窗口设计为 16 s 保持 RECONNECTING 以演示自动恢复，FAILED 路径在开发轮次中被观察并记录为设计行为。
- **低帧率源延迟波动**：10~12 fps 源使 mux 批填充稀疏（40 ms 超时主导），P95 窗口间波动 33→48 ms；Qwen 期间 FPS 无下降（实测），延迟波动不完全归因于 Qwen。
- **启动期重连计数残留**：`total_reconnects_` 成功时不回零，启动期一次失败尝试会留下 rc=1；隔离性判定采用"故障窗口内不增长"语义（实测 0 新增）。
- **Dashboard 无专用 incidents 面板**：分级结果经 incidents.jsonl 与事件流展示（需求允许"Dashboard 或日志"二选一）。

## 14. 面试 1 分钟场景介绍

> "我把 JetEdge-Agent 的十四阶段能力组装成了一个工厂场景：四路视频模拟工厂入口、停车场、人员通道和走廊，其中人员通道配置了禁区。整个链路是——边缘实时检测与追踪在本地毫秒级完成；一个人走进禁区，事件引擎只发一条 zone_entry，同一个 track 每个区域只报一次，杜绝刷屏；关键帧落盘的同时异步提交给 Qwen 复核，本地告警不等云端；复核结果进入我新增的确定性 Decision Router，按规则表分成 confirmed_alert / manual_review / archived 三级，云端失败时降级为本地规则兜底——所有决策可审计。跑场景时我故意停掉一路 RTSP 发布：该路进入 RECONNECTING 指数退避，其余三路全程零重连零失败，发布恢复后 20 秒自动回到 RUNNING 并通过 FPS 验证。然后让 Python Agent 在保证 cam1 帧率的前提下尝试降低全局 P95：它用 DeepSeek 生成候选计划，经过白名单参数校验、配置快照、双基准窗口对比，实测把 P95 从 49.2 降到 32.9 毫秒并保留，全程可回滚可审计。最后 16 分钟连续运行，6 类 JSONL 共三万一千多行逐行校验零非法，RSS 只增长 0.96%，全部单元与回归测试通过。整个过程中所有数字都来自 Jetson 实机测量，没有任何估算值。"
