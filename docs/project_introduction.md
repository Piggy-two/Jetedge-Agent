# JetEdge-Agent 项目详细介绍（简历版）

> 本文件用于简历与面试：完整项目介绍 + 分阶段详细工作 + 全项目量化指标 + 中英文简历描述示例。
> **所有数字均为 Jetson Orin Nano 8GB 实机测量**，每项均有验收文档可溯源（`docs/stage4..15`）。
> 量化事实速查表见 `docs/resume_summary.md`；本文件是它的完整展开版。

---

## 1. 项目一句话定位

> 在 Jetson Orin Nano 8GB 上，用 C++17 + GStreamer/DeepStream + TensorRT 从零构建的一套**四路实时视频边缘 AI 推理与智能运维平台**：硬件解码 → batch 推理 → 目标追踪 → 规则事件 → 云端多模态复核 → RTSP 故障恢复 → 确定性动态调度 → 安全 Control API → 可验证可回滚的本地 Agent，全部阶段实机验收通过。

项目重点不是"在 Jetson 上跑通一个 YOLO 模型"，而是：

> **在有限功耗、内存和算力约束下，构建一套多路、可观测、可调度、可恢复、可分析，并能够被 Agent 安全控制的实时边缘推理系统。**

---

## 2. 项目背景与动机

面向园区、交通路口、工厂、自动驾驶测试场等多摄像头边缘感知场景。此前已有一个基于树莓派（CPU 低算力）的实时跌倒检测系统（YOLOv8n + MoveNet + ONNX Runtime + OpenCV 多线程 Pipeline + ByteTrack + MJPEG Web + SQLite 告警闭环）。

本项目在上一项目基础上验证全新的能力维度：

| 维度 | 树莓派项目（已具备） | 本项目（新增验证） |
|---|---|---|
| 算力形态 | CPU 低算力单路 | **GPU 异构计算 + 四路并发** |
| 解码 | 软件解码 | **Jetson 硬件解码（nvv4l2decoder）** |
| 推理 | ONNX Runtime | **TensorRT FP16 / INT8 + DeepStream batch 推理** |
| 性能分析 | 无 | **ftrace / CPU Affinity / 延迟分位数（P50/P95/P99）** |
| 故障恢复 | 无 | **RTSP 断流隔离与自动恢复** |
| 资源调度 | 固定 | **确定性动态调度状态机** |
| 智能分析 | 本地规则告警 | **Qwen 多模态视觉复核 + DeepSeek 文本诊断（低频异步）** |
| 自动运维 | 无 | **安全 Agent：白名单工具 + 快照 + 验证 + 自动回滚** |

能力成长路线：`树莓派低算力边缘 AI 应用闭环 → Jetson 多路 GPU 异构推理与智能运维平台`。

---

## 3. 硬件与软件环境（全部实机核查）

| 类别 | 内容 |
|---|---|
| 硬件 | Jetson Orin Nano 8GB、NVMe SSD、主动散热、千兆网络 |
| OS | Ubuntu 22.04（L4T / JetPack，以实机核查为准） |
| 推理 | **TensorRT 10.3.0**，FP16 / INT8 |
| 视频框架 | **DeepStream 7.1.0**、**GStreamer 1.20.3**（nvv4l2decoder / nvstreammux / nvinfer / nvtracker / nvds_obj_enc） |
| 模型 | YOLO11s（Ultralytics 预训练），输入 `1x3x384x640`，输出 `1x84x5040`，FP16 主交付 |
| 语言/构建 | C++17、CMake（out-of-source `build/`）、Python3、Bash |
| 网络 | RTSP（MediaMTX v1.19.3 测试环境）、自写 HTTP/1.1 Control API（零依赖）、libcurl |
| 云端 | Qwen（通义千问 dashscope，视觉复核）、DeepSeek（文本诊断与候选计划） |
| 开发方式 | Windows 主机导出/验证模型 → Git + SCP 双通道同步 → Jetson 构建与实机验收（VS Code Remote-SSH + Claude Code） |

**工程纪律**：所有 Jetson 相关 API、路径、插件属性以实机为准（`gst-inspect-1.0`、DeepStream 样例核查），不凭记忆写 API；TensorRT Engine 一律在目标 Jetson 上构建，Windows 与 Jetson 之间用 SHA256 做端到端一致性验收。

---

## 4. 系统架构

### 4.1 总体数据流

```text
多路本地视频 / RTSP / 摄像头
                │
                ▼
           Source Manager（解复用、状态管理、断流检测、自动重连）
                │
                ▼
       nvv4l2decoder 硬件解码
                │
                ▼
         nvstreammux 多流 Batch（batch=4）
                │
                ▼
       YOLO11s TensorRT / nvinfer（FP16）
                │
                ▼
         检测后处理 / nvtracker（NvDCF）
                │
                ▼
       ROI / 越线 / 停留 / 系统事件
                │
      ┌─────────┼──────────────┐
      ▼         ▼              ▼
  可视化     Metrics       Event Store
  仪表盘      (FPS/延迟)     │
                             ▼
                   事件去重 / 合并 / 分级
                             │
                    ┌────────┴────────┐
                    ▼                 ▼
               Qwen 多模态        DeepSeek 文本
               视觉事件复核       指标与日志诊断
                    └───────┬─────────┘
                            ▼
                     Unified Analysis
                            │
                            ▼
                Adaptive Scheduler（确定性 C++ 状态机）
                            │
                            ▼
                   Safe Control Executor（Agent 白名单工具）
                            │
                            ▼
                    验证 / 保留 / 回滚
```

### 4.2 四大分层职责

| 层 | 职责 | 关键约束 |
|---|---|---|
| **数据面** | 视频接入、硬件解码、多路 Batch、TensorRT 推理、目标追踪、事件统计、结构化输出 | 实时逐帧，**不依赖任何外部服务** |
| **确定性控制面** | 队列/丢帧管理、RTSP 自动重连、推理间隔调整、流优先级、温度/负载状态机、快照、回滚 | 纯 C++ 确定性逻辑，低延迟可预测 |
| **智能分析面** | Qwen 关键帧低频视觉复核、DeepSeek 指标/错误诊断、事件摘要与候选操作计划 | 秒级或分钟级，默认异步，**API 故障不影响管道** |
| **Agent 安全执行面** | 白名单工具调用、参数校验、安全门控、修改前快照、执行后验证、失败自动回滚、完整审计 | **LLM 只产生候选计划，本地策略决定最终执行** |

### 4.3 三条链路的职责分离

```text
实时链路：YOLO → Tracker → Event Rule → 立即告警（不等待云端）
智能增强：事件路由 → 去重聚合 → Qwen 或 DeepSeek → 更新结论（异步）
自动运维：异常 → DeepSeek 候选计划 → 本地 Policy 校验 → 执行 → 验证 → 回滚
```

---

## 5. 核心技术栈

```text
语言/构建    C++17、CMake、Python3、Bash
推理          TensorRT 10.3 FP16/INT8、YOLO11s（384x640，batch=4）、自写 YOLO11 parser
视频          GStreamer 1.20.3 / DeepStream 7.1.0、nvv4l2decoder 硬件解码、nvstreammux、
             nvinfer、nvtracker（NvDCF）、nvds_obj_enc 关键帧 GPU 编码
系统          Linux 调度分析（ftrace、trace_marker、CPU Affinity、taskset）、/proc 只读采样
网络          RTSP（MediaMTX 测试环境）、自写 HTTP/1.1 Control API（零第三方依赖）、libcurl
云端          Qwen（dashscope）视觉复核、DeepSeek 文本诊断与候选计划（低频异步）
配置          YAML（streams/nvinfer/control 等配置组，启动时全量校验）
测试          ctest 9 套 + 单元测试 294+ checks + Python 41 用例 + 实机故障注入 + 2h 稳定性
```

**零依赖策略**：Control API 为自写 HTTP/1.1 服务器（零第三方依赖）、关键帧取帧用官方插件、校准器自写 C++17（零 OpenCV），所有组件保持最小依赖、可移植、可复现。

---

## 6. 核心设计原则

1. **本地优先**：规则能确定的事件不调用大模型；实时主链路完全独立于 Qwen/DeepSeek/Python/网络/Agent。
2. **分级路由**：规则事件 → 本地；视觉语义不确定 → Qwen；系统指标/日志/性能问题 → DeepSeek；高风险操作 → 分析 + 本地 Policy + 白名单工具。
3. **确定性控制**：调度状态机是纯 C++ 逻辑（非 LLM 输出），显式状态、阈值、滞回、冷却、预算。
4. **安全可逆**：一切写操作遵守"校验 → 安全门控 → 快照 → 有界修改 → 审计 → 读回验证 → 失败自动回滚"。
5. **测量驱动**：不优化没有基线的系统；每个 Benchmark 记录代码版本、输入、模型、环境、配置；只用实测数字说话。
6. **故障隔离**：单流故障单流恢复；云端、Agent 故障绝不影响实时管道。

---

## 7. 分阶段开发记录（Stage 0 → 15，全部实机验收）

### Stage 0-1：环境核查与单路硬件解码基线

- **内容**：只读核查 Jetson 实机环境（JetPack/CUDA/TensorRT/DeepStream/GStreamer 版本与插件属性），搭建 C++17/CMake 工程骨架；实现 `.h264` 裸流与 `.mp4` 容器的**单路硬件解码基线**（nvv4l2decoder），验证 decode 吞吐与 EOS/Ctrl-C 行为。
- **实测结果**：环境全部实机确认（TRT 10.3.0 / DS 7.1.0 / GStreamer 1.20.3）；解码链路稳定。

### Stage 3：YOLO11s ONNX 导出、验证与跨设备一致性

- **内容**：Windows 主机导出 YOLO11s（Ultralytics 预训练）为 ONNX，ONNX Checker / ONNX Runtime inference / NaN-Inf check 三重验证，生成 `model_info.txt`，SCP 传输到 Jetson 后做 SHA256 一致性验收。
- **实测结果**：输入 `1x3x384x640` FP32、输出 `1x84x5040`、batch=1、opset 12；三项验证全 PASSED；两端 SHA256 完全一致（`41abd2ff...e078`）。

### Stage 4：TensorRT FP16 Engine + 单路 nvinfer 检测（核心攻坚）

- **内容**：Jetson 上构建 YOLO11s FP16 Engine；**自写 YOLO11 自定义 parser**（DeepStream 官方 parser 不支持 YOLO11 输出格式）接入单路 nvinfer；修复关键缺陷 `net-scale-factor=1/255`（0-255 输入导致检测错乱）。
- **实测结果**：
  - Engine：21.81 MiB，SHA256 `c6cc41d0...a82274a`，无 warning，构建耗时 365 s；
  - 单路 720p 视频 1440 帧全部处理，每帧 8-16 个目标，bus conf=0.95 / car conf=0.94，与 Python ground truth 吻合；
  - bbox 坐标还原验证通过（640x384 推理空间 → 720p 显示空间，映射误差 <1px）；
  - EOS / Ctrl-C 优雅退出；RSS 306.6→307.4 MiB 无持续增长。

### Stage 5：四路检测 + Tracker + 结构化 JSONL + per-stream Metrics

- **内容**：派生 batch-dynamic ONNX（权重与算子不变，仅 batch 维符号化 + 6 个硬编码 Reshape 常量改 batch-relative，ORT 验证 batch=1 与原模型输出完全一致 max diff 0.0）；构建 batch=4 FP16 Engine（21.25 MiB，profile MIN=1/OPT=4/MAX=4）；四路视频 + nvstreammux + nvinfer + **nvtracker（NvDCF）** 全链路；结构化 JSONL（stream_id/track_id/class/confidence/bbox）+ per-stream input/inference/output 三阶段 FPS。
- **实测结果**：
  - 4 路不同场景视频 2072 帧全部处理（cam1 1442 / cam2 163 / cam3 288 / cam4 179），EXIT=0；
  - 满 batch 时每路 52.84 fps，**合计吞吐 ~211 fps**；
  - stream_id 映射正确（各流检测内容与各自场景匹配；同视频四路每路检测数完全一致，batch 偏移正确）；
  - track_id 跨帧稳定（cam1 最长 701 帧连续无断档）；JSONL 18,333 行；RSS 收敛 ~615 MiB 无泄漏；
  - 短流先 EOS 导致 batch 不满时，nvinfer 按实际 batch 推理无报错。

### Stage 6：事件系统 + 事件去重 + 关键帧抽取（核心攻坚）

- **内容**：5 类规则事件（appearance / disappearance / count_high / count_exit / zone_entry）+ 去重状态机（grace 15 帧、count 滞回）+ 事件 JSONL + 事件触发的整帧关键帧 JPEG。
- **关键帧取帧技术路线（本阶段最大攻关）**：NVMM 纹理 buffer 直读像素两条路线实机证伪（`gst_buffer_map` 只映射出 64 B 的 `NvBufSurface*`；CPU map 对混合 layout 的 BLOCK_LINEAR UV 平面复制失败）→ 最终改用**官方 `nvds_obj_enc`（GPU 编码任意 NVMM layout）**，经 `NVDS_CROP_IMAGE_META` 读回写文件，表面索引语义使用文档化的 `frame_meta->batch_id`。
- **实测结果**：事件 JSONL 1194 行逐行校验 **0 非法**（cam1：appearance 369 / disappearance 369 / count_high 33 / count_exit 32 / zone_entry 369）；关键帧 150 次保存 0 错误，内容与源视频同帧 SSIM=0.985（cam1）、跨流对照 -0.028（映射精确）；RSS 619.8→628.0 MiB 收敛；修复"事件 JSONL 裸值无引号致全行非法"缺陷。

### Stage 7：Qwen + DeepSeek 异步分析（三层验收）

- **内容**：事件路由（本地规则本地化 / zone_entry→Qwen 视觉复核 / 周期指标→DeepSeek 诊断）+ 有界异步优先级队列（max_size=32，满时按最低优先级+最旧丢弃，探针线程零阻塞）+ 复用的 libcurl HTTP 客户端（连接池、超时、仅瞬态错误重试、指数退避、**熔断 5/30/2**）+ 固定提示词 + jsoncpp 字段级 schema 校验。
- **实测结果（三层验收）**：
  - 单元测试：熔断器 6 组 + schema 解析 20 项全 PASS；
  - 本地 mock 端点全链路：qwen 369 + deepseek 6 请求，375 行 analysis JSONL 逐行校验 **0 失败**，管道 FPS 无影响（44.09 vs 44.07）；
  - 死端点故障注入：熔断 OPEN 后 288 请求跳过，管道 EXIT=0；
  - 线上真实 API：qwen3.6-flash（4381/9042 ms）与 deepseek-v4-flash 真实请求成功，0 解析失败；首轮暴露并修复 **qwen markdown 围栏（```json）解析缺陷**；
  - 密钥安全：env + 本地 secrets.env 运行时解析，日志只记录错误码，无 key / 无完整 Base64 泄漏。

### Stage 8：RTSP 故障隔离与恢复（核心攻坚）

- **内容**：每流独立状态机 `OFFLINE → CONNECTING → RUNNING → DEGRADED → RECONNECTING → FAILED` + 指数退避重连（1→2→4→8→15s）+ 恢复后输入 FPS 验证（5 s 窗口 / min_fps 1.0）+ 重试预算耗尽进 FAILED 停止重试风暴 + bus ERROR 按元素归属分流（流级错误单流重连）。测试环境 MediaMTX（用户目录，零系统包）。
- **实测结果**：4 路冒烟 200 s 零重连零失败（每路 ~29.3 fps）；**10 轮 cam3 停/恢复故障注入：cam1/2/4 全程 0 stall / 0 reconnect / 0 failure（460 s），cam3 每轮自动恢复**；6 次真实连续失败后进入 FAILED 停止重试；事件 JSONL 8419 行 0 非法；RSS 收敛。
- **定位并修复 2 个隐蔽缺陷**：
  1. **watchdog 下溢假 stall**：`now` 在 tick 循环开头捕获一次，前一流重建耗时后 cam4 检查用旧 `now` 减新 `last` 产生无符号下溢 → 每轮必误报假 stall；修复为每流循环内取 `now` + 下溢保护；
  2. **陈旧错误重复计数**：垂死 rtspsrc 的 bus ERROR 在重建后才到达且被重复计数 → 健康流被误判 FAILED；用元素身份校验（`is_chain_element`）修复。

### Stage 9：确定性 C++ 动态调度器（核心攻坚）

- **内容**：纯逻辑状态机 `NORMAL | PRESSURE | THERMAL | CRITICAL | RECOVERY`，带滞回、最小保持 15 s、冷却 30 s、调整预算 2 次/120 s、热优先级、CRITICAL 不增载、缺失指标不困死；每状态输出推理间隔表；只读系统采样（/proc/stat、/proc/meminfo、thermal zone 最大温度）；**decoder src 探针逐流按间隔丢帧**（计数先于丢弃，watchdog 看全速率，重建后首帧必保留）；流优先级保护（cam1 high 最后被节流）。
- **实测结果**：单测 54 checks + ctest 5/5；实机四轮运行：
  - Run A（正常负载）：全程 NORMAL，零干扰；
  - Run B（6×yes 烧机 → PRESSURE）：**精确节流 cam2 15.0 fps（=30/2）、cam4 10.0 fps（=30/3）**，高优先级流最后受影响；停负载后 RECOVERY 逐级恢复；
  - Run C（真实温度）：53.8°C → THERMAL、56.1°C → CRITICAL，预算封顶，cam4 ~2 fps 运行 0 假 stall，管道零影响；
  - Run D：闭环验证并沉淀调参规则——**滞回间隙必须大于热噪声（~0.5°C）**。

### Stage 10：ftrace / CPU Affinity 性能分析（核心攻坚）

- **内容**：60 s sched ftrace 基线（70 线程自由漂移）→ 定位解码线程调度尾部延迟 → 解码线程每流钉核 → 回归对比；对"应用线程聚堆钉核"假设实机证伪并 revert。
- **实测结果**：**wake→run 尾部延迟 p99 由 45.3 ms 降至 1.48 ms（≈30 倍改善）**，线程迁移率 0，端到端检测零回归；固化 `scripts/start_pipeline.sh`。

### Stage 11：安全 Control API、快照、验证与回滚（Agent 前置）

- **内容**：自写 HTTP/1.1 白名单 Control API（零第三方依赖，默认禁用）；写操作统一流程（参数校验 → 安全门控 CRITICAL 拒升载 → 修改前快照 → 有界修改 → 审计 JSONL → 读回验证 → 失败自动回滚；写操作互斥串行）；配置快照落盘（max 32 剪除）+ 回滚恢复全部字段；最近错误环形缓冲；所有控制操作经 `g_main_context_invoke` 派发到 GLib 主循环线程（有界等待、失败优雅降级）。
- **实测结果**：单测 206 checks + ctest 6/6；实机 4 路 RTSP 全端点 curl 验收（只读 6 类 + 写操作 7 类非法输入全拒、interval/priority/快照/回滚/restart 全过、cam4 interval=2 节流实机生效、回滚恢复全部字段、restart 恢复 RUNNING）；**8080 端口被 Open WebUI 占用时 API 优雅降级继续服务（API 故障不影响管道的实机证据）**；审计全部 success；52,112 检测行 + 3,113 事件行 0 非法；SIGINT 退出码 0。

### Stage 12：Agent 白名单工具调用、验证、审计与自动回滚（核心攻坚）

- **内容**：C++ 侧 `POST /benchmark` 受控测量窗口 + **per-frame 推理段延迟分位数（P50/P95/P99）**（input→output 探针按 frame_num 配对、ring 4096/pending 256、watermark 窗口切分）；独立 Python Agent（7 个白名单工具：get_system_metrics / get_stream_status / set_stream_priority / set_infer_interval / restart_stream / run_benchmark / rollback_config 等）、DeepSeek 低频函数调用候选计划 + **确定性执行/验证/回滚**、LLM 故障降级确定性默认策略、CRITICAL 预检/写前复查/600 s 硬 deadline/基线快照回滚 + 读回比对、**双审计链**（Agent JSONL + 服务端 §16 审计）。
- **实测结果**：ctest 7/7（control_api 255 checks + metrics_registry 29 checks）+ Python 41 用例 0 失败；实机 4 路 RTSP 端到端：
  - 场景 A（LLM 在线）：before 43.3 ms → 变更 → 42.4/41.7 ms **未达双阈值（≤36.8/35.3）自动回滚 ×2**；LLM 空候选时降级照常执行（大模型故障不影响闭环的实机证据）；
  - 场景 B（--no-llm）：cam1 FPS 保底 35 实测 29.8→26.1/24.9 触发回滚 —— **根因是 batch 填充效应**（低流量流推理频率下降反使 P95 抬高），双重指标失败自动回滚；
  - 场景 C：**Agent 被 SIGKILL，服务端 0 ERROR，4 路持续 RUNNING**，未完成变更残留已记录；
  - 服务端审计 benchmark×20 / snapshot×7 / rollback×12 / set_infer_interval×25；RSS 收敛、SIGINT 退出码 0。

### Stage 13：INT8 PTQ 与精度回归（工具链完整，按纪律回退）

- **内容**：自定义 C++17 校准器（零 OpenCV，EntropyCalibrator2 + MinMax 双算法，`apps/calib_generator`）+ 校准数据抽帧（667 帧默认 / 991 帧密集实验，4 场景与 nvinfer 同口径预处理）+ 5 个 INT8 engine 变体 Jetson 实机构建（全部 SHA256 记录）+ 自动精度回归框架（FP16/INT8 同帧对比 + ORT FP32 参考 2072 帧；accuracy_math 22 单测 + compare_precision 阈值判定）。
- **实测结论（严谨的量化验证）**：
  - **entropy2 校准证伪**：系统性压缩检测置信度（Δconf 均值 0.34、cam1 检测数 -69%、cam2 全灭）；
  - **检测头 FP16 混合精度证伪**：`--layerPrecisions=/model.23/*:fp16` 受层融合限制无效；
  - **MinMax 校准有效但未达保守阈值**：Δconf 0.034（达标）、class 一致性 0.998（达标），但匹配率 0.947/0.705/0.996/0.944 **未达 0.95 保守阈值**（未匹配 82% 集中在 0.25-0.4 低置信边缘；量化方向安全：无幻觉、无类翻转，关键目标留存 bus 0.983/car 0.960/person 0.950）；
  - **按工程纪律回退 FP16 交付**（CLAUDE.md §16 回滚纪律），INT8 性能收益实测记录在案：**P95 43.44→35.51 ms（-18.3%）**、P50 -20.2%、P99 -18.4%、drop 0；
  - 修复校准器 2 个绑定契约缺陷（batch 连续 buffer + cudaMalloc ×batch）；ctest 9/9。

### Stage 14：2 小时稳定性测试 + Demo 1-4 + 项目包装

- **2 小时稳定性运行**（4 路 RTSP，llm 关闭）：

| 指标 | 实测 |
|---|---|
| 运行时长 | **7208 s（2h00m08s）** |
| 帧数 | **873,569**（每路 ~218,000，全程 ~30 fps 无下降） |
| JSONL | 检测 3,887,981 行 + 事件 178,356 行，**逐行校验 0 非法** |
| RSS | 629.6 → 635.0 MiB（**+0.86% 收敛**，后段完全平坦） |
| 延迟（120 个 60s 采样点） | **P50 36.5 / P95 43.5 / P99 47.0 ms**，首尾窗口 P95 漂移仅 +0.3 ms |
| 温度 / CPU | 均值 63.5°C（峰值 64.2°C）/ 14.0% |
| 意外重连 / 失败 | **0 / 0**（4 路全程 RUNNING） |
| 调度 | NORMAL ×120/120（100%） |
| 退出 | SIGINT → `exit OK` |

- **Demo 1-4 实机演示**：
  - Demo 1 故障恢复：cam3 断流 → 退避重连 → 恢复后 `verified: 30.0 fps` 自动回 RUNNING，其余流 0 影响；
  - Demo 2 事件理解：本地事件 8,747 行 0 非法、关键帧 119 张、**Qwen 真实视觉复核 31 行 0 非法**（带关键帧确认；配额用尽后诚实降级拒绝、零幻觉）；云端往返 23.2 s 期间本地产出 573 事件（本地优先实证）；云端调用时 FPS 29.2-30.0 / P95 44.3 ms 零影响；
  - Demo 3 性能诊断：DeepSeek 周期诊断捕获 cam3 FPS 24.5→18.4 vs 基线 27.6 的异常并建议核查，恢复后 4 路 RUNNING；
  - Demo 4 安全 Agent：真实 DeepSeek 候选计划 → 快照 → 执行 → 双 benchmark → **自动回滚 ×4**（含确定性策略回滚 ×2），回滚后 interval 全部归 0、4 路 RUNNING，双审计链完整。
- **定位并修复 DeepSeek 空响应缺陷**：deepseek-v4-flash 默认推理把 512 token 预算全部消耗在 `reasoning_content` → `content` 为空 → schema 校验失败。线上 curl 实证两种修复方案后采用"禁用 thinking"：把配置中无效的 `thinking_mode` 旋钮真正接线（`"thinking":{"type":"disabled"}`，服务端 + Agent 同步），加回归测试 6a/6b，修复后全链路成功。

### Stage 15：Web 可视化仪表盘与一键演示

- **内容**：Control API 加 CORS（`control.cors` 配置可关）+ **零依赖静态仪表盘**（单 HTML 文件，无构建）：实时每路 FPS / 延迟分位数 / 调度状态灯 / 事件流 / 关键帧缩略图 / 安全操作面板（interval/priority/restart/快照/回滚，直接演示 §16 闭环）；经 `GET /dashboard` 服务；新增只读端点 `GET /events/recent`（64 KiB 有界尾读、整行、limit 钳位 [1,200]）、`GET /keyframes`、`GET /keyframes/{name}`（文件名白名单 `[A-Za-z0-9_-]+\.jpg`，拒绝路径穿越，5 MiB 上限）；`scripts/demo_run.sh` 一键演示编排（文件源 42 s / RTSP 持续两种模式）。
- **实测结果**：单测 294 checks（+24）+ ctest 9/9；实机浏览器直连演示通过；修复 http_server **6 处 `send_error` 不关闭连接的潜伏缺陷**（Connection: close 语义下 read-until-EOF 客户端会挂死）；管道热路径零改动。

### 开源视频复用性验证（2026-08-05）

- 4 个 intel-iot-devkit 开源视频（CC-BY 4.0，432p 带音轨，~17.6 MB）**零代码改动**跑通检测/跟踪/事件/调度/控制面；帧数与视频时长精确对应、JSONL 0 非法、0 ERROR、exit OK；
- 可疑检测（cell phone / boat / tv / chair）经 **ORT FP32 逐帧交叉验证全部归因于模型本身**（管道零发明）——用独立工具链证伪"管道可能凭空产生检测"的怀疑。

---

## 8. 关键量化指标总览（全部 Jetson 实测）

### 8.1 性能与吞吐

| 指标 | 实测值 | 出处 |
|---|---|---|
| 4 路 720p 硬件解码 + batch=4 推理 | 每路 ~30 fps，满 batch 每路 52.84 fps，**合计 ~211 fps** | Stage 5 |
| 推理段延迟（2h 全程） | **P50 36.5 / P95 43.5 / P99 47.0 ms**，漂移 +0.3 ms | Stage 12/14 |
| 解码线程调度尾部延迟 | wake→run **p99 45.3 → 1.48 ms（≈30 倍）**，迁移率 0 | Stage 10 |
| INT8 性能（实测记录） | **P95 -18.3%**（43.44→35.51 ms），P50 -20.2%，drop 0 | Stage 13 |

### 8.2 稳定性（2 小时连续运行）

| 指标 | 实测值 |
|---|---|
| 连续运行 | 7208 s，87.4 万帧，每路 ~30 fps 全程稳定 |
| 内存 | RSS +0.86% 收敛，后段完全平坦，无泄漏 |
| JSONL | 406 万行逐行校验 **0 非法** |
| 重连 / 失败 | **0 / 0**，调度 NORMAL ×120/120 |
| 退出 | SIGINT 优雅退出 `exit OK` |

### 8.3 可靠性（故障注入实测）

| 场景 | 实测值 |
|---|---|
| cam3 断流 10 轮停/恢复 | 健康流全程 0 stall / 0 reconnect / 0 failure；故障流每轮自动恢复 |
| RTSP 重连策略 | 指数退避 1→2→4→8→15 s；6 次连续失败进 FAILED 停止重试风暴 |
| 6×yes 烧机（PRESSURE） | 精确节流 cam2 15.0 fps、cam4 10.0 fps（=30/2、30/3），高优先级流最后受影响 |
| 真实温度 THERMAL→CRITICAL | 预算封顶、cam4 ~2 fps、0 假 stall、管道零影响 |
| 云端 API 死端点 | 熔断 OPEN 后 288 请求跳过，管道 FPS 无影响（44.09 vs 44.07） |
| Agent 被 SIGKILL | 服务端 0 ERROR，4 路持续 RUNNING |
| 不可达优化目标 | Agent 执行→benchmark→未达双阈值→**自动回滚**，字段全部恢复 |
| API 端口被占用 | Control API 优雅降级，管道不受影响 |

### 8.4 正确性

| 项 | 实测值 |
|---|---|
| 检测精度 | bus conf 0.955 / car conf 0.58-0.97，与 ground truth 吻合；bbox 还原误差 <1px |
| 事件系统 | 5 类规则事件 1194 行全部合法 JSON；关键帧 150 张 0 错误，SSIM 0.985 |
| LLM schema | 固定提示词 + 字段级校验，375 行 analysis 0 失败；线上真实调用通过 |
| INT8 精度回归 | 全自动框架：FP16/INT8 同帧对比 + ORT FP32 参考，结论可复现 |
| 测试体系 | ctest 9 套 + 单测 294+ checks + Python 41 用例，全绿 |

---

## 9. 重点难点攻关详解（面试可展开）

1. **YOLO11 自定义 parser 与坐标还原**（Stage 4）
   DeepStream 官方 parser 不支持 YOLO11 输出（绝对像素坐标 + 已 sigmoid class scores）。自写 parser 并实测验证 bbox 还原误差 <1px、置信度与 ground truth 吻合；修复 `net-scale-factor` 配置错误导致的检测错乱。

2. **关键帧取帧：NVMM 纹理 buffer 的像素读取**（Stage 6）
   直读像素两条路线实机证伪（gst_buffer_map 只映射出 64 B 的 NvBufSurface 指针；CPU map 遇 BLOCK_LINEAR UV 平面复制失败）→ 换用官方 `nvds_obj_enc` GPU 编码，SSIM 0.985 验证内容正确。体现"先证伪再选型、用官方已验证组件"的工程判断。

3. **RTSP 假 stall：无符号下溢的隐蔽时序 bug**（Stage 8）
   watchdog tick 循环开头捕获一次 `now`，前一流重建耗时后对下一流的检查产生无符号下溢 → 每轮必假 stall、健康流误判。修复：`now` 移入每流循环内 + 下溢保护。配套修复"垂死 rtspsrc 陈旧错误重复计数"（元素身份校验）。10 轮故障注入 0 误报。

4. **wake→run 尾部延迟：ftrace 驱动的性能定位**（Stage 10）
   60 s sched ftrace 基线发现 70 线程自由漂移 → 解码线程每流钉核，p99 45.3→1.48 ms（30 倍）；对"应用线程聚堆钉核"的假设实机证伪并 revert。完整"症状→证据→根因→受控变更→对比→保留/回退"闭环。

5. **确定性动态调度器**（Stage 9）
   滞回 / 最小保持 / 冷却 / 调整预算 / 热优先级 / CRITICAL 不增载的纯 C++ 状态机；烧机负载下精确节流（30/2、30/3 的整数倍），真实温度下 0 假 stall；沉淀"滞回间隙须 > 热噪声"的调参规则。

6. **INT8 校准路线量化验证**（Stage 13）
   不凭直觉选型：entropy2 系统性压缩置信度（Δconf 0.34）证伪；MinMax 有效（Δconf 0.034）但匹配率未达 0.95 保守阈值 → 按工程纪律回退 FP16 交付、性能收益记录在案。体现"工具链完整 + 回归框架自动化 + 决策可复现"。

7. **batch 填充效应**（Stage 12）
   低流量流推理频率下降导致 batch 不满、P95 反而升高 —— Agent benchmark 保底指标触发回滚的根因。验证了"多指标联合验证"的必要性（延迟 + FPS 双重保底）。

8. **LLM 响应契约排查：DeepSeek 空 content**（Stage 14）
   周期诊断全部 schema 校验失败 → 截断原始响应日志定位 `content` 为空、512 token 预算被 `reasoning_content` 占满（模型侧行为变化）→ 线上 curl 实证两种修复方案 → 把无效 `thinking_mode` 旋钮真正接线 + 回归测试 6a/6b。完整"症状→证据→根因→实证→最小修复→回归"闭环。

9. **自写 HTTP 服务器的隐蔽缺陷**（Stage 15）
   发现并修复 6 处 `send_error` 不关闭连接（Connection: close 语义下 read-until-EOF 客户端挂死），配套 CORS 预检、有界尾读（64 KiB）、文件名白名单等安全细节。

---

## 10. 测试与验证体系

| 层级 | 内容 | 规模 |
|---|---|---|
| 单元测试（ctest） | 配置校验、YOLO 解析、事件去重、熔断器、schema、reconnect 策略、调度器状态机、metrics、control API | **9 套 + 294+ checks 全绿** |
| Python 用例 | Agent 策略与工具参数校验 | 41 用例 0 失败 |
| 集成/实机验收 | 每阶段在 Jetson 实机跑真实视频/RTSP 全链路，EOS/Ctrl-C/内存/JSONL 逐行校验 | 15 个阶段全部通过 |
| 故障注入 | RTSP 断流 10 轮、6×yes 烧机、真实温度 THERMAL/CRITICAL、死端点熔断、Agent SIGKILL、端口占用降级 | 全部通过 |
| 长期稳定性 | 2 小时 4 路 RTSP 连续运行 | 7208 s 全程通过 |
| 独立性验证 | 4 个开源视频零代码改动复用 + ORT FP32 交叉验证 | 通过 |

验收纪律：每个阶段只有"Jetson 实机 + 真实数据"的验收通过后才标记完成；未验证功能不标记完成；只用实测数字汇报。

---

## 11. 简历描述示例

### 11.1 中文（详细版）

- 在 Jetson Orin Nano 8GB 上从零构建 C++17/DeepStream/TensorRT 四路实时视频推理平台（YOLO11s，batch=4）：实测合计 ~211 fps、推理延迟 P50 36.5 / P95 43.5 / P99 47.0 ms，通过 2 小时连续稳定性验收（87.4 万帧、RSS 仅 +0.86% 收敛无泄漏、JSONL 406 万行逐行校验 0 非法、0 意外重连）。
- 自写 YOLO11 自定义 parser 接入 nvinfer（bbox 还原误差 <1px、置信度与 ground truth 吻合）；实现 5 类规则事件 + 去重状态机 + 关键帧抽取（SSIM 0.985 内容验证，两条直读像素路线证伪后选用官方 GPU 编码方案）。
- 实现 RTSP 断流隔离与自动恢复：10 轮故障注入健康流全程 0 影响、指数退避防重试风暴；定位并修复 watchdog 无符号下溢假 stall 等 2 个隐蔽缺陷。
- 用 ftrace 定位解码线程调度尾部延迟，每流钉核后 wake→run p99 由 45 ms 降至 1.5 ms（30 倍改善）。
- 设计确定性 C++ 动态调度器（NORMAL/PRESSURE/THERMAL/CRITICAL/RECOVERY 状态机，滞回/冷却/预算/热优先级）：烧机负载下精确节流低优先级流（30/2、30/3），高优先级流最后受影响，真实温度下 0 假 stall。
- 集成 Qwen/DeepSeek 低频异步分析：事件本地即时告警、云端复核字段级 schema 校验、熔断降级，实测 API 故障对实时管道零影响。
- 实现安全 Control API（自写零依赖 HTTP/1.1）与本地 Agent：白名单工具、修改前快照、执行后验证、失败自动回滚、双审计链；Agent 被 SIGKILL 时管道持续运行，未达优化目标自动回滚 ×4。
- 完成 INT8 PTQ 工具链与自动精度回归框架（entropy2 证伪、MinMax 未达保守阈值，按工程纪律回退 FP16 交付，INT8 性能收益 -18.3% 实测记录）。
- 实现零依赖 Web 可视化仪表盘（实时 FPS/延迟分位数/调度状态/事件流/关键帧/安全操作面板）与一键演示脚本，浏览器直连实机演示通过。

### 11.2 中文（精简版，约 3-4 条）

- 从零构建 Jetson Orin Nano 8GB 上的四路实时视频推理平台（C++17 + DeepStream + TensorRT，YOLO11s batch=4），实测合计 ~211 fps、P95 延迟 43.5 ms，通过 2 小时稳定性验收（87.4 万帧、RSS +0.86% 收敛、406 万行 JSONL 0 非法、0 意外重连）。
- 实现 RTSP 断流隔离与自动恢复（10 轮故障注入健康流 0 影响）、确定性动态调度器（五态状态机，烧机下精确节流低优先级流）、ftrace 钉核优化（wake→run p99 45 ms→1.5 ms，30 倍）。
- 集成 Qwen/DeepSeek 低频异步智能分析（schema 校验 + 熔断降级，API 故障对管道零影响）与安全本地 Agent（白名单工具 + 快照 + 验证 + 自动回滚 + 双审计链）。
- 完成 INT8 PTQ 工具链与自动精度回归框架，实测性能 -18.3%；因精度未达保守阈值按工程纪律回退 FP16 交付，决策过程可复现。

### 11.3 English（英文简历可用）

- Built a 4-stream real-time edge inference platform on Jetson Orin Nano 8GB (C++17, DeepStream, TensorRT, YOLO11s): ~211 fps aggregate, P95 45 ms latency, passed a 2-hour stability run (~0.87M frames, flat RSS +0.86%, zero invalid JSONL lines, zero unexpected reconnects).
- Diagnosed decoder thread-scheduling tail latency with ftrace; pinning one decoder pair per CPU core cut wake→run p99 from 45 ms to 1.5 ms (30×).
- Designed a deterministic runtime scheduler (NORMAL/PRESSURE/THERMAL/CRITICAL/RECOVERY with hysteresis & cooldown): precise throttling of low-priority streams under CPU burn while protecting high-priority traffic; zero false stalls under real thermal pressure.
- Implemented RTSP fault isolation and recovery: per-stream state machine with exponential backoff; 10 rounds of fault injection left healthy streams completely unaffected.
- Integrated async Qwen/DeepSeek analysis with schema validation and circuit breaking: measured zero impact on the real-time pipeline during API outages.
- Built a safe Control API (hand-written zero-dependency HTTP/1.1) and local agent (whitelist tools, snapshots, post-change verification, auto-rollback, dual audit chains); pipeline survived agent SIGKILL without interruption; auto-rolled back 4 real failed optimization attempts.
- Delivered an INT8 PTQ toolchain with an automated precision-regression framework (−18.3% P95 measured); reverted to FP16 per engineering discipline when accuracy missed the conservative threshold.

### 11.4 面试可讲的点（按优先级）

1. 2 小时稳定性数据如何设计、采集与验证（P50/P95/P99 采样口径、JSONL 逐行校验、RSS 收敛判定）；
2. 三个"证伪"经历：关键帧直读像素两条路线、INT8 entropy2 校准、应用线程聚堆钉核 —— 体现测量驱动而非直觉驱动；
3. 两个隐蔽 bug 的定位过程：watchdog 无符号下溢、DeepSeek reasoning 占满 token 预算；
4. 安全 Agent 的完整闭环：为什么 LLM 只生成候选计划、写操作九步流程、双审计链、batch 填充效应；
5. 架构取舍：为什么实时链路不依赖云端/Agent；自写 HTTP 服务器而非引入框架的动机。

---

## 12. 项目时间线

```text
2026-07-31  环境核查与工程骨架；单路硬件解码基线
2026-08-01  ONNX 导出验证 + TensorRT FP16 单路/四路检测、追踪、事件关键帧
2026-08-01  Qwen/DeepSeek 异步分析（mock + 故障注入 + 线上真实 API 三层验收）
2026-08-02  RTSP 故障恢复、确定性动态调度器（实机故障注入验收）
2026-08-04  ftrace/CPU Affinity、Control API 快照回滚、Agent 闭环、INT8 工具链
2026-08-05  Stage 14 稳定性（2h/87.4 万帧/0 非法/0 重连）+ Demo 1-4 + 包装；
            Stage 15 Web 仪表盘与一键演示；开源视频复用验证
```

---

## 13. 证据文档索引

| 内容 | 文档 |
|---|---|
| 量化指标速查 | `docs/resume_summary.md` |
| 稳定性验收 | `docs/stage14_stability.md` |
| Demo 1-4 | `docs/demo.md` |
| Web 仪表盘 | `docs/stage15_plan.md` |
| 开源视频复用 | `docs/openvideo_validation.md` |
| 各阶段验收报告 | `docs/stage6_events.md`、`stage7_llm.md`、`stage8_rtsp.md`、`stage9_scheduler.md`、`stage10_ftrace.md`、`stage11_control.md`、`stage12_agent.md`、`stage13_int8.md` |
| 模型信息 | `models/model_info.txt` |
