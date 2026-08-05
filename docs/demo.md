# Stage 14 — Demo 指南（Demo 1-4）

> 状态：2026-08-05 实机验收通过（4 个 Demo 全部在 Jetson Orin Nano 8GB 实机演示并记录证据）。
> 对应 README §19 的四个预期 Demo；演示节奏参考 `docs/implementation_plan.md` §74（6~8 分钟）。

## 演示环境

```text
设备          Jetson Orin Nano 8GB（MAXN_SUPER 电源模式，主动散热）
视频源        本地 MediaMTX + ffmpeg 发布循环（scripts/rtsp_serve.sh），4 路 RTSP
               rtsp://127.0.0.1:8554/cam1..cam4（720p H.264，~30 fps/路）
模型          YOLO11s TensorRT FP16，batch=4，384x640
配置          Demo 1/2：configs/streams_stage14_demo.yaml（完整功能：RTSP+推理+追踪+事件+LLM+调度+Control API）
               Demo 3/4：configs/streams_stage14_demo3.yaml（zone→qwen 关闭，避免 cam1 事件洪峰把 DeepSeek 周期诊断挤出有界队列）
API           Control API @ 127.0.0.1:8090
依赖          真实云端 API（Qwen dashscope / DeepSeek），密钥在 ~/.jetedge/secrets.env
```

准备：

```bash
./scripts/rtsp_serve.sh status          # 4 路正在发布
./scripts/start_pipeline.sh configs/streams_stage14_demo.yaml    # Demo 1/2
./scripts/start_pipeline.sh configs/streams_stage14_demo3.yaml   # Demo 3/4
./scripts/start_pipeline.sh stop        # 演示结束后停止
```

---

## Demo 1：四路视频基础能力 + 故障恢复（~4 分钟）

**讲点**：多路硬件解码、batch 推理、逐流状态机；单流故障不影响其他流。

| 步骤 | 操作 | 预期证据 |
|---|---|---|
| 1a | `curl -s :8090/streams` | 4 路全部 `RUNNING`，`reconnect_count=0`、`consecutive_failures=0` |
| 1b | `curl -s :8090/metrics/summary` | 每路 `latest_input_fps≈30`，`lat_p95_ms≈45` |
| 1c | 查看 `logs/jetedge_server.log` 周期报告 | per-stream input/inference/output FPS 稳定 |
| 1d | `./scripts/rtsp_serve.sh cam-stop cam3` | 日志：cam3 `DEGRADED/RECONNECTING`，指数退避 1→2→4→8→15s；`curl :8090/streams` 显示 cam1/2/4 全程 `RUNNING`、0 重连 0 失败 |
| 1e | 约 20~30s 后 `./scripts/rtsp_serve.sh cam-start cam3` | cam3 自动恢复 `RUNNING`，`verify_sec=5` FPS 验证通过，`reconnect_count=1`、`last_reason` 记录原因 |
| 1f | 停止前 `SIGINT`（`start_pipeline.sh stop`） | 优雅退出，退出码 0 |

**验收点**：故障注入期间健康流零影响（0 stall / 0 reconnect / 0 failure）。

---

## Demo 2：事件理解（本地规则 → 关键帧 → Qwen 视觉复核，~3 分钟）

**讲点**：实时告警本地即刻产生，不等待云端；Qwen 只做低频语义复核。

| 步骤 | 操作 | 预期证据 |
|---|---|---|
| 2a | 运行期间查看 `logs/stage14_demo_events.jsonl` 尾部 | 本地规则事件（appearance/count_high/zone_entry）时间戳持续前进，**先于**任何 Qwen 响应 |
| 2b | `ls logs/keyframes_stage14_demo/` | 事件触发的关键帧 JPEG 持续落盘（cap 150） |
| 2c | 等待首个 Qwen 复核（每个 zone_entry 事件路由，HTTP 4~9s） | `logs/stage14_demo_analysis.jsonl` 出现 `provider=qwen` 行，字段 `description/risk/confidence` 通过 schema 校验 |
| 2d | 展示一条完整 analysis 行 | 真实模型输出场景描述与风险判断（JSON，无 markdown 围栏） |
| 2e | `curl :8090/streams` | 云端调用期间 4 路 FPS 无下降（API 不影响实时链路） |

**验收点**：本地告警时延 < 1s；Qwen 响应正确解析并入库；管道 FPS 无影响。

---

## Demo 3：性能诊断（故障注入 → DeepSeek 聚合诊断，~4 分钟）

**讲点**：DeepSeek 不看原始日志，只看本地聚合指标与错误摘要；周期诊断低频触发。

| 步骤 | 操作 | 预期证据 |
|---|---|---|
| 3a | 运行至少 60s（DeepSeek 周期 `deepseek_interval_sec=60`） | 首个周期诊断行（`provider=deepseek`）正常 |
| 3b | `./scripts/rtsp_serve.sh cam-stop cam3`，保持 ~90s | 期间 cam3 重连/失败计数上升；管道其余流正常 |
| 3c | 观察下一个周期诊断 | analysis 行聚合了 reconnect/failure 计数与 FPS 变化，给出原因判断与候选操作（JSON schema 校验通过） |
| 3d | `./scripts/rtsp_serve.sh cam-start cam3` | 恢复；诊断后续周期引用恢复状态 |

**验收点**：DeepSeek 诊断引用真实聚合指标；错误不泄漏原始日志；API 故障不影响管道（熔断保护）。

---

## Demo 4：安全 Agent（候选计划 → 快照 → 执行 → Benchmark → 验证 → 保留/回滚，~8 分钟）

**讲点**：Agent 不参与逐帧控制；所有写操作走 CLAUDE.md §16 流程；LLM 只生成候选计划。

| 步骤 | 操作 | 预期证据 |
|---|---|---|
| 4a | 查询基线：`curl :8090/metrics/summary` | P50≈37ms / P95≈45ms / P99≈55ms |
| 4b | 场景 A（保留）：`python3 agent/main.py --goal "在保证 cam1 推理 FPS 不低于 25 的前提下,降低全局 P95 延迟"` | Agent：查询指标 → DeepSeek 候选计划（`set_infer_interval cam4 1` 等）→ 参数校验 → 快照 → 执行 → `POST /benchmark` → 对比 → 达标**保留**；审计 JSONL 出现 `snapshot`+`set_infer_interval`+`benchmark` 记录 |
| 4c | 场景 B（回滚）：`python3 agent/main.py --goal "把全局 P95 延迟降低到 20ms 以下"` | 目标不可达（P95 已 ~45ms，`p95_trivial_ms=20`）→ 执行后 benchmark 不达标 → **自动回滚**；审计出现 `rollback` 记录；`curl :8090/scheduler/config` 字段与执行前一致 |
| 4d | 全程 `curl :8090/streams` | 4 路始终 RUNNING，Agent 故障不影响管道 |
| 4e | `kill -9 <agent pid>`（可选故障注入） | 服务端 0 ERROR，未完成变更残留已记录 |

**验收点**：保留/回滚两条路径均有 before/after 实测数据；快照、审计双链完整；管道全程运行。

---

## 演示讲解脚本（6~8 分钟，参考 implementation_plan §74）

```text
0:00-1:00  问题与架构：多路边缘推理的难点（算力/内存/热/网络）；
           三层架构（数据面 / 确定性控制面 / 智能分析面）；Agent 不参与逐帧控制。
1:00-2:30  Demo 1 多路推理：4 路视频检测框 + track_id + 每路 FPS + 全局 P95。
2:30-3:30  Demo 1 故障恢复：停 cam3 → 其余流继续 → 自动重连 → FPS 恢复验证。
3:30-5:30  Demo 2 事件理解：本地告警即时、关键帧、Qwen 场景描述与风险判断。
5:30-7:00  Demo 3 性能诊断：注入故障 → DeepSeek 聚合诊断 + 候选计划。
7:00-8:00  Demo 4 安全 Agent：执行 → Benchmark → 验证保留；不可达目标自动回滚。
```

---

## 验收汇总（2026-08-05 实测）

| Demo | 验收结果 |
|---|---|
| Demo 1 | 4 路 ~30 fps；cam3 断流 → 指数退避重连（reconnect 2→4→5）→ 发布恢复后 `verified: 30.0 fps ≥ 1.0 → RUNNING`；**cam1/2/4 全程 0 reconnect / 0 failure**；SIGINT 优雅退出 |
| Demo 2 | 本地事件 8,747 行 0 非法 + 关键帧 119 张落盘；Qwen 真实复核 31 行 0 非法——**带关键帧的复核确认成功**（"A white car is detected..."，confirmed=true）；关键帧 cap 后的降级复核诚实拒绝（"cannot verify without image"，confirmed=false，零幻觉）；Qwen 23.2s 云端往返期间本地继续产出 573 条事件（本地优先）；云端调用时 FPS 29.2-30.0、P95 44.3ms 零影响 |
| Demo 3 | DeepSeek 周期诊断全部 success：基线诊断（cam1 目标密度异常）、故障期诊断（cam3 FPS 24.5→18.4 vs 其余 27.6，建议核查网络/帧率）、恢复后诊断（4 路 RUNNING，cam3 低活动度分析）——均基于真实聚合指标，JSON 合法 |
| Demo 4 | 场景 A（真实 DeepSeek）：候选计划 2 轮（interval 调整）→ before P95 43.6 → after 42.2/40.5ms 未达双阈值（≤37.1/35.6）→ **自动回滚 ×2**；场景 B（--no-llm 确定性）：P95 44.2→42.0/41.5 + cam1 FPS 26.0/25.6 < 35 保底 → **自动回滚 ×2**；两次运行后 4 路 interval 全部归 0、RUNNING 全程；服务端审计 6 benchmark / 8 set_infer_interval / 4 rollback / 2 snapshot 全记录；Agent 审计 31 行 0 非法 |

## 验收期间发现并修复的缺陷

- **DeepSeek 响应为空（2026-08-05）**：`deepseek-v4-flash` 默认启用推理（reasoning），512 token 输出预算被 reasoning_content 全部消耗，`choices[0].message.content` 返回空字符串 → schema 校验失败（实机日志 + 原始响应实证：`"content":"","reasoning_content":"We need to analyze the metrics..."`）。线上 curl 实证两种修复均有效：max_tokens=2048（reasoning 2542 字符后仍输出 JSON）与 `"thinking":{"type":"disabled"}`（reasoning_content=0，512 预算足够）。按 §14"常规诊断优先非思考模式 + 输出有界"采用后者，把配置里长期无效的 `thinking_mode` 旋钮真正接线（服务端 `build_deepseek_body` + Agent `deepseek_client.py` 同步修复），回归测试 6a/6b 新增。修复后 DeepSeek 周期诊断与 Agent 规划全部成功。
