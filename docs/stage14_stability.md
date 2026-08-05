# Stage 14 — 2 小时稳定性测试、Demo 与项目包装（验收报告）

> 状态：2026-08-05 实机验收通过（Jetson Orin Nano 8GB）。
> 本文所有数字均为实机测量；Demo 详见 `docs/demo.md`；简历指标汇总见 `docs/resume_summary.md`。

## 1. 测试目标（简历导向指标）

1. 2 小时连续 4 路 RTSP 实时推理：RSS 收敛无泄漏、JSONL 逐行 0 非法、0 意外重连、P95 稳定、优雅退出。
2. Demo 1-4 全链路实机演示（四路基础能力+故障恢复 / 事件理解+Qwen / 性能诊断+DeepSeek / 安全 Agent+自动回滚）。
3. 项目包装：演示指南、阶段报告、README 快照、简历指标汇总。

## 2. 环境与配置

| 项 | 值 |
|---|---|
| 设备 | Jetson Orin Nano 8GB，NVMe SSD，主动散热 |
| 电源模式 | MAXN_SUPER |
| 视频源 | 本地 MediaMTX + ffmpeg 4 路发布循环（`scripts/rtsp_serve.sh`），720p H.264 ~30fps/路 |
| 模型 | YOLO11s TensorRT FP16 batch=4 384x640（engine SHA256 见 `docs/stage5`） |
| 配置 | `configs/streams_stage14.yaml`（RTSP+推理+追踪+事件+调度+Control API；llm 关闭——理由见该配置头部注释：2h 内 cam1 zone 事件会产生数万次真实 Qwen 调用，LLM 韧性已在 Stage 7/12 验收并由 Demo 2-4 覆盖） |
| 采样 | `scripts/stability_monitor.py`，60s 间隔，120 个样本 |
| 运行窗口 | 2026-08-05 10:00:26 → 12:00:34 CST（7208 s） |

## 3. 2 小时运行结果

### 3.1 总览

| 指标 | 实测 | 目标 | 结论 |
|---|---|---|---|
| 运行时长 | **7208 s（2h00m08s）** | 7200 s | ✅ 达标 |
| 处理帧数（4 路合计） | **873,569**（cam1 218,341 / cam2 218,568 / cam3 218,330 / cam4 218,330） | ~84 万 | ✅ |
| 检测 JSONL 行数 | **3,887,981**（cam1 2.60M / cam2 0.62M / cam3 0.18M / cam4 0.48M） | — | 逐行校验 0 非法 |
| 事件 JSONL 行数 | **178,356**（appearance 57,192 / disappearance 57,192 / zone_entry 53,868 / count_high 5,053 / count_exit 5,051） | — | 逐行校验 0 非法 |
| 意外重连 / 失败 | **0 / 0**（4 路全程 RUNNING） | 0 / 0 | ✅ |
| 调度状态 | **NORMAL ×120 采样（100%）** | NORMAL | ✅ 无 PRESSURE/THERMAL/CRITICAL |
| RSS 起始 → 结束 | **629.6 → 635.0 MiB（+0.86%）**，峰值 635.0 MiB | 收敛 | ✅ 无泄漏 |
| 温度 | 均值 63.5°C / 峰值 64.2°C | 远离 THERMAL 阈值 75°C | ✅ |
| 进程 CPU（单核计） | 均值 14.0% / 峰值 15.1% | — | 稳定 |
| 退出 | SIGINT → `exit OK`（日志）+ 监控确认进程退出 | exit 0 | ✅ 优雅 |

### 3.2 延迟稳定性（每 60s 采样，4 路一致）

| 分位 | 均值 | 最小 | 最大 | 首尾 10% 窗口漂移 |
|---|---|---|---|---|
| P50 | 36.5 ms | 36.2 | 37.1 | — |
| P95 | 43.5 ms | 43.0 | 45.0 | **+0.3 ms** |
| P99 | ~47.0 ms | — | — | — |

2 小时内延迟零漂移（<1 ms），与 Stage 12 实机 P95 45-46 ms 一致。

### 3.3 每路 FPS（60s 采样）

| 流 | 平均 | 最小 | 最大 |
|---|---|---|---|
| cam1 | 30.0 | 29.0 | 31.0 |
| cam2 | 30.1 | 29.0 | 30.9 |
| cam3 | 30.0 | 29.0 | 31.0 |
| cam4 | 30.0 | 29.0 | 30.9 |

无随时间下降（首尾窗口对比无差异）。

### 3.4 关键帧

- 150 次保存（达 max_keyframes cap，与 Stage 6 同口径），120 个唯一文件（同毫秒多事件同名覆盖，已知口径），0 错误。

## 4. 关键检查项

| 检查 | 结果 | 证据 |
|---|---|---|
| 内存是否持续线性增长 | **否（+0.86%，后段完全平坦）** | 120 样本 CSV：t=6848→7148 仅 +180 kB |
| FPS 是否随时间下降 | 否 | 首尾窗口 29.9→29.9 fps |
| 重连后资源是否释放 | 无重连发生 | 4 路 reconnect_count=0 |
| 指标线程是否卡死 | 否 | 5s 周期报告持续输出至 12:00:34 |
| JSONL 有效性与完整性 | **0 非法 / 406 万行** | `validate_jsonl.py` 逐行校验 PASS（events 178,356 + detections 3,887,981） |
| 关键帧写入 | 150 次保存 0 错误 | `logs/keyframes_stage14/` 120 文件 |
| 调度器状态正确性 | NORMAL ×120（100%） | 采样 CSV `sched_state` |
| SIGINT 优雅退出 | `exit OK` | 日志 12:00:34 记录 |

## 5. Demo 1-4 结果

见 `docs/demo.md` 验收汇总表（2026-08-05 实机演示通过）与缺陷修复记录（DeepSeek 空响应）。

## 6. 打包产物

| 产物 | 路径 | 入库 |
|---|---|---|
| 稳定性配置 | `configs/streams_stage14.yaml` | ✅ |
| Demo 配置（llm 开启，Demo 2） | `configs/streams_stage14_demo.yaml` | ✅ |
| Demo 分析配置（Demo 3/4，zone→qwen 关闭避免洪峰挤掉 DeepSeek） | `configs/streams_stage14_demo3.yaml` | ✅ |
| 稳定性采样器 | `scripts/stability_monitor.py` | ✅ |
| 采样分析器 | `scripts/analyze_stability.py` | ✅ |
| JSONL 校验器 | `scripts/validate_jsonl.py` | ✅ |
| 采样原始数据 | `logs/stage14_stability_samples.csv` | ❌（不入 Git，汇总见本文 §3） |
| 运行日志 / JSONL | `logs/jetedge_server.log`、`logs/stage14_*.jsonl` | ❌ |
| Demo 指南 | `docs/demo.md` | ✅ |
| 简历指标汇总 | `docs/resume_summary.md` | ✅ |

## 7. 验收期间发现并修复的缺陷

**DeepSeek 响应内容为空（R1）**：`deepseek-v4-flash` 模型侧行为变化——默认启用推理，512 token 输出预算被 `reasoning_content` 全部消耗，`choices[0].message.content` 返回空字符串，导致全部 DeepSeek 调用 schema 校验失败（服务端日志 + 原始响应实证：`"content":"","reasoning_content":"We need to analyze the metrics..."`）。线上 curl 实证两种修复均有效（max_tokens=2048 或 `"thinking":{"type":"disabled"}`）；按 §14"常规诊断优先非思考模式 + 输出有界"采用思考禁用参数，把配置中此前无效的 `thinking_mode` 旋钮真正接线（`src/llm/prompt_manager.cpp` / `llm_router.cpp` / `agent/deepseek_client.py` 同步），新增回归测试 6a/6b（ctest 9/9）。修复后 Demo 3 周期诊断与 Demo 4 Agent 规划全部 success。修复过程符合 §10 根因流程：症状 → 截断响应日志 → 根因 → 线上实证 → 最小修复 → 回归测试。

## 8. 遗留与说明

- 2 小时运行 llm 关闭（设计决策，见 §2）；Demo 2-4 在短窗口内以真实 API 覆盖完整链路。
- 关键帧 120 唯一文件 < 150 次保存为同毫秒覆盖口径（Stage 6 已记录），非错误。
- 关键帧 cap（150）后 zone_entry 事件路由 Qwen 时降级为纯元数据复核（提示词显式说明 "No image is available"），Qwen 诚实返回低置信而非幻觉——有界资源设计下的正确降级行为，非缺陷。
- 电源模式 MAXN_SUPER + 主动散热下温度稳定 63-64°C，未触发 THERMAL（调度器 NORMAL 全程）；高温触发路径由 Stage 9 Run C 实测覆盖。
- RTSP FAILED 为终态（防重试风暴设计，Stage 8），Control API 拒绝重启 FAILED 流（RTSP_FAILED），恢复需重建管道——Demo 3 中如实呈现。
