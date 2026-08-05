# JetEdge-Agent — 简历项目描述与量化指标汇总

> 目的：给简历项目描述提供一份**全部实机测量**（Jetson Orin Nano 8GB）的量化事实表。
> 所有数字均有对应验收文档（docs/stage4..14）。未测量数字不写入。

## 1. 一句话项目故事

> 在 Jetson Orin Nano 8GB 上，用 C++17 + GStreamer/DeepStream + TensorRT 从零构建了一套**四路实时视频推理平台**：硬件解码、batch 推理、目标追踪、规则事件、云端多模态复核、RTSP 故障恢复、确定性动态调度、安全 Control API，以及一个可验证、可回滚的本地 Agent —— 全部 14 个阶段在实机验收通过。

## 2. 技术栈

```text
语言/构建    C++17、CMake、Python3、Bash
推理          TensorRT 10.3 FP16/INT8、YOLO11s（384x640，batch=4）、自写 YOLO11 parser
视频          GStreamer 1.20 / DeepStream 7.1、nvv4l2decoder 硬件解码、nvstreammux、
             nvinfer、nvtracker(NvDCF)、nvds_obj_enc 关键帧
系统          Linux 调度（ftrace、CPU Affinity、taskset）、/proc 指标采样
网络          RTSP（MediaMTX 测试环境）、自写 HTTP/1.1 Control API、libcurl
云端          Qwen（dashscope）视觉复核、DeepSeek 文本诊断与候选计划
测试          ctest 单测 9 套 + 单测 200+ checks + Python 41 用例 + 实机故障注入
```

## 3. 核心量化指标（全部 Jetson 实测）

### 3.1 性能与吞吐

| 指标 | 实测值 | 出处 |
|---|---|---|
| 4 路 720p 硬件解码 + batch=4 推理 | 每路 ~30 fps，**合计 ~211 fps**（满 batch 每路 52.84 fps） | Stage 5 |
| 推理段延迟（input→output 探针） | **P50 36.8ms / P95 45.0ms / P99 55.5ms**，2h 全程稳定 | Stage 12/14 |
| 解码线程调度尾部延迟 | wake→run **p99 45.3ms → 1.48ms（≈30 倍改善）**，线程迁移 0 | Stage 10 |
| INT8 性能（工具链完整，精度未达保守阈值按纪律回退 FP16） | **P95 43.44→35.51ms（-18.3%）**，drop 0 | Stage 13 |

### 3.2 稳定性（Stage 14，2 小时连续运行实测）

| 指标 | 实测值 |
|---|---|
| 连续运行 | **7208 s（2 小时）**，4 路 RTSP 同时推理，**87.4 万帧**（每路 ~30 fps 全程稳定） |
| 推理延迟 2h 全程 | **P50 36.5ms / P95 43.5ms / P99 47.0ms**，首尾 10% 窗口 P95 漂移仅 +0.3ms |
| RSS | **629.6 → 635.0 MiB（+0.86%）**，后段完全平坦，收敛无泄漏 |
| JSONL | 检测 **388.8 万行 + 事件 17.8 万行**（合计 406 万行），**逐行校验 0 非法** |
| 意外重连/失败 | **0 / 0**（4 路全程 RUNNING） |
| 调度器 | NORMAL 全程（120/120 采样），温度 63.5°C 均值，远离 THERMAL 阈值 |
| 退出 | SIGINT 优雅退出，`exit OK` |

### 3.3 可靠性（故障注入实测）

| 场景 | 实测值 | 出处 |
|---|---|---|
| cam3 断流 10 轮停/恢复 | cam1/2/4 全程 **0 stall / 0 reconnect / 0 failure**；cam3 每轮自动恢复 | Stage 8 |
| Demo 1-4 全链路 | 断流自动恢复（30.0fps 验证）、Qwen 视觉复核（带关键帧确认 + 降级诚实拒绝零幻觉）、DeepSeek 故障诊断、Agent 双阈值未达自动回滚 ×4，管道全程运行 | Stage 14 |
| RTSP 重连策略 | 指数退避 1→2→4→8→15s，6 次连续失败进 FAILED 停止重试风暴 | Stage 8 |
| 6×yes 烧机（PRESSURE） | 精确节流 cam2 15.0 fps、cam4 10.0 fps（=30/2、30/3），高优先级 cam1 最后被节流 | Stage 9 |
| 真实温度 THERMAL→CRITICAL | 预算封顶、cam4 ~2fps 运行、0 假 stall、管道零影响 | Stage 9 |
| 云端 API 死端点 | 熔断 OPEN 后 288 请求跳过，**管道 FPS 无影响**（44.09 vs 44.07） | Stage 7 |
| Agent 被 SIGKILL | 服务端 0 ERROR，4 路持续 RUNNING | Stage 12 |
| 不可达目标 | Agent 执行→benchmark→未达双阈值→**自动回滚**，字段全部恢复 | Stage 12/14 |

### 3.4 正确性

| 项 | 实测值 |
|---|---|
| 检测精度 | bus conf 0.955 / car conf 0.58-0.97，与 ground truth 吻合；bbox 还原误差 <1px | Stage 4/5 |
| 事件系统 | 5 类规则事件 1194 行全部合法 JSON；关键帧 150 张 0 错误，SSIM 0.985 | Stage 6 |
| LLM schema | 固定提示词 + 字段级校验，375 行 analysis 0 失败；线上 qwen/deepseek 真实调用通过 | Stage 7 |
| INT8 精度回归 | 全自动框架：FP16/INT8 同帧对比 + ORT FP32 参考；结论可复现 | Stage 13 |
| 测试体系 | ctest 9 套 + 单测 200+ checks + Python 41 用例，全绿 | Stage 4-13 |

## 4. 代表性难点攻关（面试可讲）

1. **YOLO11 自定义 parser**：DeepStream nvinfer 官方 parser 不支持 YOLO11 输出（绝对像素坐标 + 已 sigmoid class scores），自写 parser 并验证坐标还原 <1px。
2. **关键帧取帧**：NVMM 纹理 buffer 直读像素两套方案实机证伪（layout 混合/UV 复制失败），改用官方 `nvds_obj_enc` GPU 编码，SSIM 0.985 验证内容正确。
3. **RTSP 假 stall**：watchdog 时间戳在循环开头捕获导致无符号下溢 → 每轮必误报；修复后 10 轮故障注入 0 误报。
4. **wake→run 尾部延迟**：ftrace 定位 70 线程自由漂移 → 解码线程每流钉核，p99 45ms→1.5ms。
5. **INT8 校准路线验证**：entropy2 系统性压缩置信度（Δconf 0.34）证伪；MinMax 有效但匹配率 0.947 未达保守阈值 → 按工程纪律回退 FP16 交付，性能收益记录在案。
6. **batch 填充效应**：低流量流推理频率下降反而抬高 P95（batch 不满），Agent benchmark 保底指标触发回滚的根因。
7. **Agent 安全闭环**：LLM 只生成候选计划；本地确定性执行/验证/回滚，写操作走快照+审计+读回比对。
8. **LLM 响应契约排查**：DeepSeek 周期诊断全部 schema 校验失败 → 截断原始响应日志定位 `content` 为空、输出预算被 `reasoning_content` 占满（模型侧行为变化）→ 线上 curl 实证修复方案 → 把配置中无效的 `thinking_mode` 旋钮接线 + 回归测试，修复后全链路成功。完整"症状→证据→根因→实证→最小修复→回归"闭环。

## 5. 简历 bullet 示例

### 中文

- 在 Jetson Orin Nano 8GB 上从零构建 C++17/DeepStream/TensorRT 四路实时视频推理平台（YOLO11s，batch=4），实测合计 ~211 fps、推理延迟 P50 36.5/P95 43.5/P99 47.0ms，并通过 2 小时连续稳定性验收（87.4 万帧、RSS 增长仅 0.86% 收敛无泄漏、JSONL 406 万行逐行校验 0 非法、0 意外重连）。
- 实现 RTSP 断流隔离与自动恢复（10 轮故障注入健康流 0 影响、指数退避防重试风暴），并用 ftrace 定位解码线程调度尾部延迟，每流钉核后 wake→run p99 由 45ms 降至 1.5ms（30 倍）。
- 设计确定性 C++ 动态调度器（NORMAL/PRESSURE/THERMAL/CRITICAL/RECOVERY 状态机，滞回/冷却/预算），烧机负载下精确节流低优先级流、高优先级流最后受影响。
- 集成 Qwen/DeepSeek 低频异步分析：事件本地即时告警、云端复核 schema 校验、熔断降级，实测 API 故障对管道零影响。
- 实现安全 Control API 与本地 Agent：白名单工具、修改前快照、执行后验证、失败自动回滚，Agent 被 SIGKILL 时管道持续运行。
- 完成 INT8 PTQ 工具链与自动精度回归框架，实测性能 -18.3%；因精度未达保守阈值按工程纪律回退 FP16 交付，结论可复现。

### English

- Built a 4-stream real-time edge inference platform on Jetson Orin Nano 8GB (C++17, DeepStream, TensorRT, YOLO11s): ~211 fps aggregate, P95 45 ms latency, passed a 2-hour stability run (~0.8M frames, flat RSS, zero invalid JSONL lines, zero unexpected reconnects).
- Diagnosed decoder thread-scheduling tail latency with ftrace; pinning one decoder pair per CPU core cut wake→run p99 from 45 ms to 1.5 ms (30×).
- Designed a deterministic runtime scheduler (NORMAL/PRESSURE/THERMAL/CRITICAL/RECOVERY with hysteresis & cooldown): precise throttling of low-priority streams under CPU burn while protecting high-priority traffic.
- Integrated async Qwen/DeepSeek analysis with schema validation and circuit breaking: measured zero impact on the real-time pipeline during API outages.
- Built a safe Control API and local agent (whitelist tools, snapshots, post-change verification, auto-rollback); pipeline survived agent SIGKILL without interruption.
- Delivered an INT8 PTQ toolchain with an automated precision-regression framework (−18.3% P95); reverted to FP16 per engineering discipline when accuracy missed the conservative threshold.

## 6. 里程碑（时间线）

```text
2026-07-31  环境核查与工程骨架；单路硬件解码基线
2026-08-01  ONNX 导出验证 + TensorRT FP16 单路/四路检测、追踪、事件关键帧
2026-08-01  Qwen/DeepSeek 异步分析（mock + 故障注入 + 线上真实 API 三层验收）
2026-08-02  RTSP 故障恢复、确定性动态调度器（实机故障注入验收）
2026-08-04  ftrace/CPU Affinity、Control API 快照回滚、Agent 闭环、INT8 工具链
2026-08-05  Stage 14：2 小时稳定性（7208s/87.4 万帧/0 非法/0 重连）+ Demo 1-4 + 项目包装；发现并修复 DeepSeek reasoning 空响应缺陷
```
