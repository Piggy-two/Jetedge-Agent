# JetEdge-Agent：Jetson 多路视频边缘 AI 推理与智能运维平台

> 面向秋招项目展示的优化版方案：突出一条可落地、可测量、可解释、可演示的系统主线，并将开发任务拆解为可逐阶段交给 Claude Code 执行的工程步骤。

\---

## 0\. 先说结论：这个项目应该讲成什么故事

这个项目不应该讲成：

> 我在 Jetson 上跑了 YOLO、DeepStream、Grafana、ftrace 和 Agent，还接了很多功能。

而应该讲成：

> 我在 Jetson Orin Nano 8GB 的有限算力和功耗约束下，搭建了一套 4 路视频边缘推理平台。最初固定配置在多路并发、网络抖动和热压力下会出现尾延迟升高、丢帧和单流掉线问题，因此我先建立分阶段性能观测体系，再实现确定性的动态调度与故障恢复，最后将这些能力封装为受限工具，让 Agent 能根据自然语言目标执行“查询指标—修改配置—运行验证—达标保留—失败回滚”的完整闭环。

这条故事的核心递进关系是：

```text
多路视频能跑
    ↓
多路视频能稳定跑
    ↓
系统问题能够被准确测量
    ↓
瓶颈能够通过指标和 Trace 定位
    ↓
系统能够根据负载自动调整
    ↓
Agent 能在安全边界内完成目标驱动调优
    ↓
所有优化都有前后数据，失败能够回滚
```

最终面试官应当记住三个关键词：

1. **多路 GPU 异构推理平台**；
2. **可观测、可调度、可恢复**；
3. **Agent 不是聊天界面，而是安全的系统运维控制层**。

\---

# 第一部分：项目定位与范围收敛

## 1\. 项目名称

### 推荐名称

**JetEdge-Agent：基于 Jetson 的多路视频边缘 AI 推理与智能运维平台**

### 推荐副标题

> C++ / GStreamer / DeepStream / TensorRT 多路推理、性能观测、动态调度、故障恢复与 Agent 闭环调优

### 简历上的短名称

**Jetson 多路视频边缘 AI 与 Agent 智能运维平台**

\---

## 2\. 项目解决的真实问题

园区、道路、工厂和自动驾驶测试场通常需要同时处理多路摄像头。与单路视频 Demo 不同，多路边缘推理会面临以下问题：

* 多路视频同时解码和推理，GPU、CPU、内存带宽竞争明显；
* 不同视频流的网络状态、码率和业务优先级不同；
* Batch 可以提高吞吐，但也会增加等待时间和尾延迟；
* RTSP 单流异常可能拖累整条 Pipeline；
* 温度、功耗和系统负载会随时间变化；
* 固定推理间隔和固定 Batch 配置难以同时保证吞吐、延迟与关键流优先级；
* 人工查看日志、修改配置和重复 Benchmark 成本高，且容易误操作。

因此项目的目标不是只把模型部署到 Jetson，而是构建一套：

```text
多路视频接入
+ GPU 批量推理
+ 目标追踪与事件统计
+ 指标采集与瓶颈定位
+ 动态资源调度
+ 单流故障恢复
+ Agent 工具调用、验证与回滚
```

的完整边缘 AI 系统。

\---

## 3\. 与树莓派项目形成能力升级

|树莓派跌倒检测|JetEdge-Agent|
|-|-|
|单路或少路视频|4 路为核心，8 路压力测试|
|CPU 端完整应用闭环|GPU 异构、多流推理平台|
|OpenCV + ONNX Runtime|GStreamer / DeepStream + TensorRT|
|固定推理流程|Batch、流优先级与动态推理间隔|
|简单 FPS 统计|分阶段延迟、尾延迟、队列、功耗和温度|
|应用功能导向|系统吞吐、稳定性和资源调度导向|
|固定规则告警|Agent 查询、实验、验证和自动回滚|

能力成长可以总结为：

> 第一个项目证明我能在低算力 CPU 上跑通边缘 AI 应用；第二个项目证明我能设计 GPU 多流推理系统，并完成性能分析、运行时调度和智能运维闭环。

\---

## 4\. 必须收紧的项目边界

### 4.1 主线必须完成

1. C++ 多路视频 Pipeline；
2. Jetson 硬件解码；
3. TensorRT FP16 推理；
4. 4 路 Batch、追踪和结构化结果；
5. FPS、P50/P95/P99、队列和丢帧监控；
6. CPU、GPU、内存、温度和功耗监控；
7. 单路 RTSP 断流隔离与恢复；
8. 动态推理间隔和流优先级；
9. 一条 ftrace 或 CPU Affinity 的性能分析案例；
10. Control API；
11. Agent 查询、调优、验证和回滚闭环；
12. 至少 2 小时稳定性测试；
13. 一组可信的优化前后实验数据。

### 4.2 有时间再做

* TensorRT INT8 PTQ；
* Grafana Dashboard；
* 自动 Batch Benchmark；
* 事件关键帧 VLM 复核；
* Docker 化部署；
* CUDA 自定义后处理。

### 4.3 当前不要做

* 多 Agent；
* 大型 RAG；
* 长期记忆；
* 跨摄像头 ReID；
* Linux 内核模块；
* 自定义完整 GStreamer 插件；
* 多任务模型；
* 四路视频逐帧 VLM；
* 任意 Shell 执行型 Agent。

这些功能会让项目变宽，但不会让主故事更有说服力。

\---

# 第二部分：最终系统架构

## 5\. 三层架构

```text
┌─────────────────────────────────────────────────────────────┐
│                    Agent 智能决策面                         │
│ 自然语言目标 → 查询工具 → 生成计划 → 执行工具 → 验证 → 回滚 │
└──────────────────────────┬──────────────────────────────────┘
                           │ HTTP / Unix Domain Socket
┌──────────────────────────▼──────────────────────────────────┐
│                    确定性控制面                             │
│ Metrics Registry / Scheduler / Stream Manager / Snapshot    │
│ Control Server / Benchmark Runner / Audit Log               │
└──────────────────────────┬──────────────────────────────────┘
                           │ C++ API / Runtime Config
┌──────────────────────────▼──────────────────────────────────┐
│                    实时视频数据面                           │
│ Source → HW Decode → StreamMux → TensorRT → Tracker         │
│ → Analytics → Metadata / Display / Event                    │
└─────────────────────────────────────────────────────────────┘
```

### 数据面

负责毫秒级和逐帧任务：

* 视频接入；
* 解码；
* Batch；
* 推理；
* Tracker；
* ROI/越线/计数；
* 队列和丢帧策略。

### 控制面

负责确定性、低风险的运行时控制：

* 指标聚合；
* RTSP 重连；
* 状态机调度；
* 流优先级；
* 推理间隔；
* 配置快照；
* 参数校验；
* 回滚。

### Agent 面

负责秒级或分钟级目标驱动任务：

* 理解“降低延迟”“保障 cam1”“诊断 cam3”；
* 查询真实指标；
* 选择白名单工具；
* 执行受限实验；
* 对比前后结果；
* 失败时调用回滚；
* 生成可读报告。

### 关键设计边界

> Agent 不直接接触 DeepStream 内部对象，不参与逐帧决策，也不允许执行任意 Shell。它只能调用经过参数限制、可审计、可回滚的 Control API。

这条边界是项目设计中最重要的面试点之一。

\---

## 6\. 推荐 Pipeline

### 本地文件阶段

```text
filesrc
→ qtdemux
→ h264parse
→ nvv4l2decoder
→ queue
→ nvstreammux
→ nvinfer
→ nvtracker
→ nvdsanalytics
→ metadata exporter
→ fakesink / tiler + OSD
```

### RTSP 阶段

```text
rtspsrc / nvurisrcbin
→ jitter buffer
→ depay
→ parse
→ nvv4l2decoder
→ queue
→ nvstreammux
→ nvinfer
→ nvtracker
→ analytics
```

### 输出分支

```text
                           ┌→ fakesink：纯性能测试
tracker / analytics → tee ├→ tiler + OSD：Demo 展示
                           └→ metadata exporter：JSON / Metrics
```

第一版不要先做录像、VLM 和复杂 Web UI。

\---

# 第三部分：开发策略——如何正确使用 Claude Code

## 7\. Claude Code 在项目中的角色

Claude Code 适合承担：

* 阅读现有代码和配置；
* 创建工程骨架；
* 实现单个边界清晰的模块；
* 补充单元测试和脚本；
* 分析编译错误和日志；
* 重构重复代码；
* 更新 README 和设计文档；
* 根据实际命令输出修复问题。

Claude Code 不应该替你决定：

* 项目最终范围；
* 实机环境是否兼容；
* DeepStream 插件是否真实存在；
* 性能是否真的提升；
* 指标是否可信；
* 实验是否公平；
* 简历中填写什么数据。

你的角色是：

> 架构负责人 + 实机测试负责人 + 验收负责人。

Claude Code 的角色是：

> 在明确接口、输入、输出和验收条件下完成工程实现。

\---

## 8\. 不要一次让 Claude Code 生成整个项目

错误方式：

```text
帮我实现一个 Jetson 多路视频 DeepStream TensorRT Agent 平台。
```

这种提示会导致：

* 一次生成大量不可运行代码；
* 使用不存在的 API；
* 模块边界混乱；
* 编译错误难以定位；
* Agent 和底层 Pipeline 同时开发，问题互相干扰；
* 看起来功能很多，但没有可验收的里程碑。

正确方式：

```text
环境核查
→ 单路最小程序
→ 4 路本地视频
→ 推理与追踪
→ 指标采集
→ RTSP 恢复
→ 动态调度
→ Control API
→ Agent 闭环
```

每次只给 Claude Code 一个可在 0.5～2 天内完成的任务。

\---

## 9\. 推荐 Claude Code 工作循环

每个任务统一采用下面的循环：

```text
1. 先让 Claude Code 只读分析仓库和环境输出
2. 让它给出修改计划和涉及文件
3. 你确认任务边界
4. 让它实现最小改动
5. 在 Jetson 上编译或运行
6. 把真实报错完整交给 Claude Code
7. 修复后执行验收命令
8. 记录结果和提交 Git
9. 再进入下一阶段
```

建议每个阶段至少保留一个独立 Git Commit：

```text
feat: add single-stream deepstream baseline
feat: support four local video sources
feat: add per-stream metrics registry
feat: add rtsp reconnect state machine
feat: add adaptive inference scheduler
feat: expose safe control api
feat: add agent validation and rollback loop
```

\---

## 10\. 项目根目录中的 CLAUDE.md

在开始编码前，先创建 `CLAUDE.md`，让 Claude Code 每次都遵守项目约束。

建议内容：

```markdown
# JetEdge-Agent Development Rules

## Project Goal
Build a stable and measurable multi-stream video inference platform on Jetson Orin Nano 8GB using C++17, GStreamer/DeepStream and TensorRT. Add deterministic scheduling and a tool-calling Agent only after the base pipeline is stable.

## Hard Constraints
- Do not run or recommend `sudo apt update`.
- Do not install system packages without explicit approval.
- Prefer read-only environment inspection first.
- Do not assume DeepStream, TensorRT or GStreamer versions; inspect the target device.
- Do not invent APIs. Verify headers, samples and installed plugin properties locally.
- Do not implement multiple project stages in one change.
- Do not let the Agent execute arbitrary shell commands.
- All runtime write operations must validate parameters, create snapshots, write audit logs and support rollback.
- Keep the real-time C++ pipeline independent from LLM availability.

## Engineering Style
- C++17 and CMake.
- RAII for GStreamer objects where practical.
- No global mutable state unless justified.
- Structured logs with module, stream\_id, state and error\_code.
- Configuration lives in YAML/JSON files, not hard-coded values.
- Add tests for pure logic modules.
- Every feature must include run instructions and acceptance criteria.

## Workflow
1. Inspect first.
2. Propose a small change plan.
3. List files to create or modify.
4. Implement only the current task.
5. Provide build and test commands.
6. Report assumptions and unresolved environment dependencies.
```

\---

# 第四部分：推荐仓库结构

## 11\. 初始结构不要过度设计

第一阶段只创建：

```text
jetedge-agent/
├── CMakeLists.txt
├── CLAUDE.md
├── README.md
├── apps/
│   └── jetedge\_server/
│       └── main.cpp
├── include/jetedge/
│   ├── common/
│   └── pipeline/
├── src/
│   ├── common/
│   └── pipeline/
├── configs/
│   ├── streams.yaml
│   └── pipeline.yaml
├── scripts/
│   └── check\_environment.sh
├── tests/
└── docs/
    └── development\_log.md
```

随着阶段推进再增加：

```text
include/jetedge/metrics
include/jetedge/scheduler
include/jetedge/control
include/jetedge/stream
src/metrics
src/scheduler
src/control
src/stream
agent/
benchmark/
monitoring/
```

不要在第一天创建 `cuda/`、`kernel/`、`plugins/`、`vlm/` 和 `rag/`。

\---

# 第五部分：分阶段实施步骤

下面每个阶段都包含：

* 目标；
* 具体任务；
* Claude Code 提示词；
* 你需要手工执行的命令；
* 验收标准；
* 阶段产物。

务必按顺序推进。

\---

# 阶段 0：只读环境核查与项目初始化

## 12\. 阶段目标

确认 Jetson 上真实安装的版本、插件、示例和编译条件，避免 Claude Code 根据通用知识生成与实机不兼容的代码。

## 13\. 执行步骤

### Step 0.1：创建工作目录

```bash
mkdir -p \~/projects/jetedge-agent
cd \~/projects/jetedge-agent
git init
```

### Step 0.2：只读核查环境

禁止执行 `sudo apt update`。

核查内容：

```bash
cat /etc/os-release
uname -a
head -n 1 /etc/nv\_tegra\_release 2>/dev/null || true

dpkg-query -W nvidia-jetpack 2>/dev/null || true
nvcc --version 2>/dev/null || true
trtexec --version 2>/dev/null || /usr/src/tensorrt/bin/trtexec --version 2>/dev/null || true
deepstream-app --version-all 2>/dev/null || true

gst-inspect-1.0 --version
gst-inspect-1.0 nvv4l2decoder 2>/dev/null | head -n 40
gst-inspect-1.0 nvstreammux 2>/dev/null | head -n 60
gst-inspect-1.0 nvinfer 2>/dev/null | head -n 60

nvpmodel -q --verbose 2>/dev/null || true
cat /proc/device-tree/model 2>/dev/null || true
free -h
df -h
cmake --version
g++ --version
python3 --version
docker --version 2>/dev/null || true
```

### Step 0.3：查找本机 DeepStream 示例

```bash
find /opt/nvidia/deepstream -maxdepth 4 -type d -name '\*samples\*' 2>/dev/null | head
find /opt/nvidia/deepstream -maxdepth 6 -type f \\( -name '\*.cpp' -o -name '\*.c' \\) 2>/dev/null | head -50
```

不要让 Claude Code凭记忆猜 API；应优先参考本机样例和头文件。

### Step 0.4：保存环境快照

把输出保存到：

```text
docs/environment\_snapshot.md
```

内容包括：

* JetPack / L4T；
* CUDA；
* TensorRT；
* DeepStream；
* GStreamer；
* 编译器；
* 功耗模式；
* 可用内存和磁盘；
* 插件是否存在；
* 本机样例路径。

## 14\. 给 Claude Code 的提示词

```text
你现在只做阶段 0，不要实现视频 Pipeline。

任务：
1. 阅读当前仓库、CLAUDE.md 和我提供的 Jetson 环境核查输出；
2. 创建最小 C++17 + CMake 工程骨架；
3. 创建 scripts/check\_environment.sh，只包含只读检查命令；
4. 创建 docs/environment\_snapshot.md 模板；
5. 创建 README.md，写明当前阶段目标和构建方式；
6. 不安装任何系统依赖，不执行或建议执行 sudo apt update；
7. 不假设 DeepStream 和 TensorRT 版本；
8. 完成后列出创建的文件、构建命令和仍需确认的环境依赖。

先给出修改计划和文件列表，再实施。
```

## 15\. 验收标准

* `cmake -S . -B build` 成功；
* `cmake --build build -j2` 成功；
* 生成一个仅打印版本信息的可执行程序；
* 环境快照完整；
* Git 提交：

```text
chore: initialize jetedge project and environment audit
```

\---

# 阶段 1：单路本地视频最小 Pipeline

## 16\. 阶段目标

只解决一件事：

> 使用 C++ 在 Jetson 上正确读取一段本地 H.264/MP4 视频，调用硬件解码，并稳定送入 fakesink。

先不接模型、Tracker、RTSP、Metrics 和 Agent。

## 17\. 执行步骤

### Step 1.1：准备本地测试视频

优先使用：

* 一段 30～60 秒 H.264 MP4；
* 1080p 或 720p；
* 固定帧率；
* 已确认可用 `gst-launch-1.0` 播放。

先通过命令验证插件链路。例如具体 demux 和 parse 以实际视频编码为准：

```bash
gst-launch-1.0 -v \\
  filesrc location=/data/videos/cam1.mp4 ! \\
  qtdemux ! h264parse ! nvv4l2decoder ! \\
  fakesink sync=false
```

### Step 1.2：实现 PipelineBuilder

建议类：

```text
PipelineConfig
PipelineBuilder
GstRuntime
BusMessageHandler
```

第一版接口：

```cpp
bool initialize();
bool build(const PipelineConfig\& config);
bool start();
void stop();
```

### Step 1.3：实现 GstBus 处理

至少处理：

* ERROR；
* EOS；
* WARNING；
* STATE\_CHANGED。

日志必须包含：

```text
module
pipeline\_state
error\_code
message
```

### Step 1.4：支持优雅退出

处理 `SIGINT`：

```text
Ctrl+C
→ 设置退出标志
→ Pipeline 切换到 NULL
→ 释放资源
→ 进程正常退出
```

## 18\. 给 Claude Code 的提示词

```text
只实现阶段 1：单路本地视频硬件解码到 fakesink。

已知环境信息位于 docs/environment\_snapshot.md。请先阅读本机 DeepStream/GStreamer 示例和当前 CMake 配置，不要猜测插件属性。

要求：
1. 使用 C++17 和 GStreamer API；
2. 输入路径从命令行或 configs/pipeline.yaml 获取；
3. Pipeline 为 filesrc → demux → parse → nvv4l2decoder → fakesink；
4. 动态 pad 正确连接；
5. GstBus 处理 ERROR、EOS、WARNING、STATE\_CHANGED；
6. Ctrl+C 能优雅退出；
7. 使用结构化日志；
8. 不加入推理、Tracker、RTSP、Metrics 和 Agent；
9. 提供构建命令、运行命令和最小验收步骤；
10. 先列出设计和文件修改，再编码。
```

## 19\. 验收标准

* 视频能够从头跑到尾；
* 没有 CPU 软件解码误用；
* EOS 后正常退出；
* 路径错误时输出明确错误；
* Ctrl+C 正常释放 Pipeline；
* 连续重复运行 10 次无崩溃；
* Git 提交：

```text
feat: add single-stream hardware decode baseline
```

\---

# 阶段 2：四路本地视频与 StreamMux

## 20\. 阶段目标

将单路 Pipeline 扩展为 1、2、4 路本地视频，完成 Source 管理和 `nvstreammux` Batch，但暂不急于加入复杂模型。

## 21\. 核心设计

每路视频使用独立 Source Bin：

```text
SourceBin\[0] ─┐
SourceBin\[1] ─┼→ nvstreammux → fakesink
SourceBin\[2] ─┤
SourceBin\[3] ─┘
```

建议类：

```text
SourceConfig
SourceBin
SourceManager
PipelineBuilder
```

每路必须有唯一：

```text
stream\_id
source\_index
uri/path
priority
expected\_fps
```

## 22\. 执行步骤

### Step 2.1：配置文件驱动

`configs/streams.yaml` 示例：

```yaml
streams:
  - id: cam1
    type: file
    uri: /data/videos/cam1.mp4
    priority: high
  - id: cam2
    type: file
    uri: /data/videos/cam2.mp4
    priority: normal
  - id: cam3
    type: file
    uri: /data/videos/cam3.mp4
    priority: normal
  - id: cam4
    type: file
    uri: /data/videos/cam4.mp4
    priority: low
```

### Step 2.2：实现 Request Pad 生命周期

必须明确：

* 请求 `nvstreammux` sink pad；
* 保存 pad 句柄；
* Source 移除时释放 request pad；
* 失败时不泄漏 GStreamer 对象。

### Step 2.3：先跑无推理 Batch Baseline

记录：

* 1 路总 FPS；
* 2 路总 FPS；
* 4 路总 FPS；
* CPU 使用率；
* GPU 使用率；
* 内存占用。

这组数据是后续 Benchmark 的 B1 基线。

## 23\. 给 Claude Code 的提示词

```text
只实现阶段 2：将现有单路本地视频程序扩展为可配置的 1/2/4 路 Source Bin，并接入 nvstreammux，输出到 fakesink。

要求：
1. 每路 Source 使用独立对象管理；
2. 正确处理 demux 动态 pad；
3. 正确请求和释放 nvstreammux request pad；
4. streams.yaml 描述 stream\_id、路径和优先级；
5. 单路创建失败时给出 stream\_id 和失败阶段；
6. 当前阶段不加入推理、Tracker、RTSP 和 Agent；
7. 增加每路收到帧数和总 FPS 的最小统计；
8. 提供 1、2、4 路运行命令；
9. 说明资源释放策略；
10. 为配置解析和 stream\_id 校验添加测试。

先阅读现有实现，再给出最小增量修改方案。
```

## 24\. 验收标准

* 1、2、4 路均可运行；
* 各流能正确映射到 source index；
* 任一路路径错误时错误信息明确；
* 4 路播放完整视频无死锁；
* 无明显内存持续增长；
* 输出每路帧数和总 FPS；
* Git 提交：

```text
feat: support four local streams with nvstreammux
```

\---

# 阶段 3：TensorRT FP16 检测、Tracker 与结构化结果

## 25\. 阶段目标

完成真正的多路 AI 数据面：

```text
4 路视频
→ 硬件解码
→ nvstreammux
→ TensorRT FP16
→ Tracker
→ 结构化检测和追踪结果
```

## 26\. 模型策略

第一版只选一个轻量检测模型，不同时适配多个模型。

推荐原则：

* 类别以车辆、行人、骑行者为主；
* 输入尺寸优先 640×384 或实测合适尺寸；
* 第一目标是稳定接入 DeepStream；
* Engine 在目标 Jetson 上构建；
* 先 FP16，再考虑 INT8；
* 不在当前阶段实现 CUDA 自定义 NMS。

## 27\. 执行步骤

### Step 3.1：ONNX 验证

在开发主机完成：

```text
PyTorch 模型
→ 导出 ONNX
→ 检查输入输出节点
→ 使用固定图片验证输出形状
```

保存：

```text
models/onnx/
models/configs/
```

### Step 3.2：在 Jetson 构建 FP16 Engine

Engine 命名必须包含：

```text
模型名 + batch + precision + input\_shape + device/version
```

例如：

```text
yolo\_b4\_fp16\_640x384\_orin\_nano.engine
```

### Step 3.3：接入 nvinfer

必须验证：

* batch-size 与实际配置关系；
* 输入 shape；
* parser；
* class 数量；
* bbox 坐标；
* confidence threshold；
* unique component id。

### Step 3.4：接入 Tracker

第一版只选一种 Tracker，先追求稳定。

记录：

* 检测帧率；
* 跟踪帧率；
* ID 是否跨帧稳定；
* infer interval > 0 时 Tracker 是否补偿未推理帧。

### Step 3.5：输出 JSONL

每帧或按固定频率输出：

```json
{
  "timestamp\_ns": 0,
  "stream\_id": "cam1",
  "frame\_id": 123,
  "objects": \[
    {
      "track\_id": 45,
      "class\_id": 0,
      "confidence": 0.91,
      "bbox": \[100, 80, 220, 300]
    }
  ]
}
```

不要一开始接数据库和消息队列。

## 28\. 给 Claude Code 的提示词

```text
只实现阶段 3：在现有四路本地视频 Pipeline 中接入一个已准备好的 TensorRT FP16 检测模型、Tracker 和 JSONL Metadata 输出。

我会提供：
- ONNX/Engine 路径；
- nvinfer 配置；
- 模型输入输出信息；
- 本机 DeepStream 示例路径。

要求：
1. 不负责训练模型；
2. 先检查现有 nvinfer 配置和本机样例；
3. 增加 nvinfer 和一个 Tracker；
4. 从 NvDsBatchMeta 中读取 stream/frame/object metadata；
5. 映射 source index 到 stream\_id；
6. 输出 JSONL；
7. 输出模型初始化失败、parser 失败和 metadata 异常的明确日志；
8. 当前不加入 RTSP、动态调度、Grafana 和 Agent；
9. 提供无显示模式；
10. 给出 1 路和 4 路验收步骤。

不要重写已稳定的 SourceManager，采用最小增量修改。
```

## 29\. 验收标准

* 4 路均有检测结果；
* stream\_id 映射正确；
* Tracker ID 正常；
* JSONL 格式稳定；
* 无显示模式可运行；
* 能配置 `infer\_interval`；
* 运行 30 分钟无崩溃；
* Git 提交：

```text
feat: add tensorrt inference tracker and metadata export
```

\---

# 阶段 4：完整 Metrics 与分层 Benchmark

## 30\. 阶段目标

这是项目从“能运行”升级为“能优化”的关键阶段。

在没有可靠指标前，不开发动态调度和 Agent。

## 31\. Metrics 分层

### 视频流指标

* `input\_fps`；
* `decoded\_fps`；
* `inference\_fps`；
* `output\_fps`；
* `drop\_count`；
* `queue\_depth`；
* `stream\_state`；
* `reconnect\_count`。

### 延迟指标

推荐在 Buffer Metadata 中保存时间戳：

```text
source\_received\_ts
batch\_ready\_ts
infer\_begin\_ts
infer\_end\_ts
tracker\_done\_ts
output\_ts
```

计算：

* source → output 端到端延迟；
* Batch 等待时间；
* 推理阶段延迟；
* Tracker 阶段延迟；
* P50 / P95 / P99。

### Batch 指标

* batch\_size\_configured；
* batch\_size\_actual；
* batch\_fill\_ratio；
* batch\_wait\_ms。

### 系统指标

从 `tegrastats` 或可靠接口采集：

* CPU 每核利用率；
* GPU 利用率；
* RAM；
* 温度；
* 功耗；
* CPU/GPU 频率；
* throttling 状态。

## 32\. Metrics 架构

建议：

```text
Pad Probe / System Collector
        ↓
MetricsRegistry
        ↓
RollingWindow / Histogram
        ↓
Console Summary / JSON / HTTP Exporter
```

实时 Pipeline 中只做低开销记录；聚合和输出在独立线程中完成。

## 33\. 分层 Benchmark

至少实现：

|编号|Pipeline|
|-|-|
|B0|decode → fakesink|
|B1|decode → streammux → fakesink|
|B2|decode → inference → fakesink|
|B3|decode → inference → tracker → fakesink|
|B4|完整 Pipeline，无 OSD|
|B5|完整 Pipeline + OSD|
|B6|完整 Pipeline + Metrics|

每组固定：

* 相同视频；
* 相同运行时长；
* 相同功耗模式；
* 相同模型；
* 相同输入尺寸；
* 前 30 秒预热不计入结果。

## 34\. 给 Claude Code 的提示词

```text
只实现阶段 4：为当前四路推理 Pipeline 增加低开销 MetricsRegistry、滚动统计和分层 Benchmark 输出。

要求：
1. 先梳理各阶段可插入 pad probe 的位置；
2. 定义统一 MetricName 和 stream\_id 维度；
3. 统计每路 input/inference/output FPS；
4. 统计端到端延迟的 P50/P95/P99；
5. 统计 batch 实际填充数量和等待时间；
6. 独立线程解析系统指标，不能阻塞实时 Pipeline；
7. 每 5 秒输出一次摘要；
8. 程序退出时输出 JSON 格式 Benchmark 报告；
9. 增加 B0-B6 模式配置，但不要在同一进程自动切换所有模式；
10. 为纯统计逻辑添加单元测试；
11. 当前不开发调度和 Agent。

请重点说明时间戳如何随 buffer/batch 传递，以及如何避免指标采集本身影响实时性能。
```

## 35\. 验收标准

* 每路 FPS 可区分；
* P50/P95/P99 不为空且逻辑合理；
* 统计线程不阻塞 Pipeline；
* B0～B6 可通过配置运行；
* 输出机器可读 JSON 报告；
* 开启 Metrics 前后性能差异可测；
* Git 提交：

```text
feat: add pipeline metrics and layered benchmark modes
```

\---

# 阶段 5：RTSP Source Manager 与故障恢复

## 36\. 阶段目标

实现单流故障隔离：

> cam3 断流时，cam1、cam2、cam4 继续运行；cam3 按受控策略重连，恢复后重新接入。

这是项目稳定性的核心亮点。

## 37\. Source 状态机

```text
OFFLINE
  ↓
CONNECTING
  ↓
RUNNING
  ↓ error/timeout
DEGRADED
  ↓
RECONNECTING
  ├→ RUNNING
  └→ FAILED
```

每个流独立维护：

* 当前状态；
* 最近一帧时间；
* 连续失败次数；
* 总重连次数；
* 最近错误码；
* 下次重连时间；
* 当前退避间隔。

## 38\. 重连策略

建议：

```text
1s → 2s → 4s → 8s → 15s，上限 15s
```

必须包含：

* 最大连续快速重试次数；
* 指数退避；
* 恢复后计数重置；
* 重连期间不阻塞主线程；
* 释放旧 request pad 和 Source Bin；
* 重建失败时不影响其他流。

## 39\. 故障注入

`scripts/inject\_fault.sh` 可支持：

* 停止一路 RTSP 推流；
* 延迟恢复；
* 修改错误地址；
* 反复开关单路源。

第一版不必模拟复杂网络丢包，只需先证明断流隔离和恢复。

## 40\. 给 Claude Code 的提示词

```text
只实现阶段 5：将 SourceManager 扩展为支持多路 RTSP，并实现单流独立状态机、断流检测和自动重连。

要求：
1. 保留本地文件输入；
2. 每路 RTSP 有独立状态；
3. 错误日志必须包含 stream\_id、state、error\_code 和 retry\_count；
4. 使用受控指数退避；
5. 重连不能阻塞 GStreamer 主循环；
6. 正确移除旧 Source Bin、unlink 并释放 request pad；
7. cam3 故障不能使其他流停止；
8. Metrics 中增加 stream\_state、last\_frame\_age\_ms、reconnect\_count；
9. 增加可重复的故障注入脚本和测试说明；
10. 当前不加入 Agent 自动诊断，仅完成确定性恢复。

先重点分析动态移除和重新添加 Source 时的 GStreamer 生命周期风险，再给出实施方案。
```

## 41\. 验收标准

* 4 路 RTSP 正常接入；
* 关闭 cam3 后其他三路持续运行；
* cam3 恢复推流后自动接入；
* 反复故障 10 次无崩溃；
* request pad 无持续泄漏；
* 错误和恢复日志结构化；
* Git 提交：

```text
feat: add isolated rtsp reconnect state machine
```

\---

# 阶段 6：确定性动态调度器

## 42\. 阶段目标

在固定配置基线之上，根据实时负载和业务优先级自动调整推理策略。

Agent 仍然不参与实时决策。

## 43\. 调度器输入

```cpp
struct RuntimeState {
    double gpu\_util;
    double cpu\_util;
    double temperature\_c;
    double power\_w;
    double p95\_latency\_ms;
    double drop\_rate;
    int queue\_depth;
    int active\_streams;
};
```

每路状态：

```cpp
struct StreamRuntimeState {
    std::string stream\_id;
    StreamPriority priority;
    double input\_fps;
    double inference\_fps;
    double p95\_latency\_ms;
    int infer\_interval;
};
```

## 44\. 调度状态

|状态|触发|动作|
|-|-|-|
|NORMAL|指标正常|使用默认配置|
|PRESSURE|P95、队列或丢帧升高|降低低优先级流推理频率|
|THERMAL|温度持续过高|关闭 OSD，进一步降低低优先级流|
|CRITICAL|严重过热或队列失控|保留关键流，暂停低优先级推理|
|RECOVERY|指标恢复|分阶段恢复，禁止一次性全部升载|

## 45\. 必须实现的防抖机制

* 进入阈值和退出阈值不同；
* 最小状态保持时间；
* 修改冷却时间；
* 每次最多修改一到两个流；
* 关键流最小推理 FPS；
* Critical 状态禁止增加负载；
* 配置变化写日志。

## 46\. 第一版调度动作

只实现两个动作：

```text
set\_infer\_interval(stream\_id, 0..5)
set\_stream\_priority(stream\_id, high|normal|low)
```

可选：

```text
enable\_osd(false)
```

不要第一版同时修改 Batch Size、分辨率、模型和功耗模式。

## 47\. 给 Claude Code 的提示词

```text
只实现阶段 6：基于现有 MetricsRegistry 增加确定性的 C++ AdaptiveScheduler。

要求：
1. 状态为 NORMAL/PRESSURE/THERMAL/CRITICAL/RECOVERY；
2. 配置文件定义阈值、滞回、最小保持时间和冷却时间；
3. 第一版只允许调整 infer\_interval，并按 stream priority 选择调整对象；
4. high 优先级流有最小 inference FPS 约束；
5. Critical 状态禁止提高负载；
6. 每次状态变化和动作写结构化日志；
7. Scheduler 与 Pipeline 通过明确接口交互；
8. 为状态转换和边界条件写单元测试；
9. 提供固定配置与动态调度的 A/B 测试方法；
10. 当前不加入 LLM 或 Agent。

请先给出状态转换表、配置 schema 和测试用例，再实现代码。
```

## 48\. 验收标准

至少证明一项：

* 相同关键流 FPS 约束下，P95 延迟下降；
* 高负载时丢帧率下降；
* 高温情况下保持更长稳定运行；
* cam1 的推理频率优先得到保障。

必须提供：

```text
固定策略数据
vs
动态调度数据
```

Git 提交：

```text
feat: add deterministic adaptive inference scheduler
```

\---

# 阶段 7：ftrace 与 CPU Affinity 性能分析案例

## 49\. 阶段目标

不是为了堆 Linux 名词，而是完成一条真实的性能定位故事：

```text
发现 P95 延迟异常
→ Metrics 判断不是纯 GPU 推理问题
→ trace\_marker 标记关键阶段
→ ftrace 观察线程调度和抢占
→ 调整 CPU Affinity 或线程策略
→ 对比优化前后数据
```

## 50\. 用户态 Trace 点

建议标记：

```text
FRAME\_RECEIVED
BATCH\_READY
INFER\_BEGIN
INFER\_END
TRACK\_DONE
FRAME\_OUTPUT
```

每个事件包含：

```text
stream\_id
frame\_id
monotonic\_timestamp
thread\_id
```

## 51\. 分析重点

* 解码回调线程是否频繁迁核；
* Metrics 或日志线程是否抢占关键线程；
* Batch 等待是否来自慢流；
* OSD 是否造成明显 CPU 开销；
* 推理线程前后是否存在长时间 sched delay；
* CPU Affinity 是否降低尾延迟。

## 52\. Claude Code 任务边界

Claude Code 可以：

* 实现 TraceMarker 封装；
* 在指定节点插入事件；
* 编写采集脚本；
* 编写 Trace 解析脚本；
* 输出 CSV 和摘要。

你必须亲自：

* 在 Jetson 实机采集；
* 判断线程名称和 PID；
* 确认所需权限；
* 解读 Trace；
* 决定是否调整 Affinity；
* 验证优化结果。

## 53\. 给 Claude Code 的提示词

```text
只实现阶段 7 的用户态 Trace 支持和分析脚本，不修改内核，也不安装系统包。

要求：
1. 实现可开关的 TraceMarker 封装；
2. 在 FRAME\_RECEIVED、BATCH\_READY、INFER\_BEGIN、INFER\_END、TRACK\_DONE、FRAME\_OUTPUT 插入标记；
3. 关闭 Trace 时额外开销应尽量小；
4. scripts/collect\_trace.sh 只生成采集命令和输出目录，不假设 sudo 权限一定存在；
5. scripts/analyze\_trace.py 将应用标记整理为 CSV；
6. 文档说明如何联合观察 sched\_switch 和 sched\_wakeup；
7. 不自动修改 CPU Affinity；
8. 增加一个可配置线程 affinity 的实验开关；
9. 输出 A/B 实验模板。

先分析当前线程模型和适合插入标记的位置。
```

## 54\. 验收标准

* Trace 中能看到应用关键阶段；
* 能将一个 frame/batch 的阶段串起来；
* 完成一次 Affinity 开启/关闭对比；
* 输出 P95/P99、线程迁移或调度等待变化；
* 即使没有提升，也要给出原因和证据；
* Git 提交：

```text
perf: add trace markers and cpu affinity experiment
```

\---

# 阶段 8：Control Server、配置快照和回滚

## 55\. 阶段目标

在开发 Agent 前，先把底层能力封装成安全工具。

Control Server 是 Agent 与 C++ Pipeline 的唯一边界。

## 56\. 第一版 API

### 只读接口

```text
GET /health
GET /metrics/summary
GET /streams
GET /streams/{stream\_id}
GET /scheduler/config
GET /scheduler/state
GET /errors/recent
```

### 写接口

```text
POST /streams/{stream\_id}/infer-interval
POST /streams/{stream\_id}/priority
POST /streams/{stream\_id}/restart
POST /config/snapshot
POST /config/rollback
POST /benchmark/run
```

## 57\. 写操作统一流程

```text
接收请求
→ 身份/策略检查
→ 参数 schema 校验
→ 读取当前状态
→ 创建配置快照
→ 执行动作
→ 返回 request\_id 和 snapshot\_id
→ 写审计日志
```

## 58\. 快照内容

```json
{
  "snapshot\_id": "snap\_001",
  "created\_at": "...",
  "reason": "before set\_infer\_interval",
  "streams": {
    "cam1": {"priority": "high", "infer\_interval": 0},
    "cam2": {"priority": "normal", "infer\_interval": 1}
  },
  "scheduler": {
    "enabled": true,
    "state": "NORMAL"
  }
}
```

第一版快照可保存在本地 JSON 文件或 SQLite，但接口必须独立，便于后续替换。

## 59\. 参数限制示例

```text
infer\_interval: 0..5
priority: high|normal|low
单次最多修改 2 路
cam1 不能被设置为 disabled
Critical 状态禁止减小 infer\_interval
restart 单流最小间隔 30 秒
```

## 60\. 给 Claude Code 的提示词

```text
只实现阶段 8：为现有 C++ Pipeline 增加安全的 Control Server、配置快照、回滚和审计日志。

要求：
1. Agent 尚未开发，API 先通过 control\_cli 或 curl 测试；
2. 只暴露白名单接口；
3. 写操作统一执行参数校验、快照、动作和审计；
4. infer\_interval 限制为 0..5；
5. priority 仅允许 high/normal/low；
6. Critical 状态禁止扩大负载；
7. rollback 必须恢复所有受影响字段；
8. 所有响应包含 success、request\_id、timestamp、data/error\_code、snapshot\_id；
9. 为参数校验、快照和回滚添加测试；
10. 不提供任意命令执行接口；
11. Control Server 异常不能使视频 Pipeline 退出。

请先给出 API schema、错误码和事务式写操作流程，再实现。
```

## 61\. 验收标准

* CLI/curl 能查询真实指标；
* 能修改 infer interval；
* 非法参数被拒绝；
* 修改前生成快照；
* rollback 能恢复原值；
* API 线程崩溃或请求错误不影响 Pipeline；
* 审计日志包含前值、后值和结果；
* Git 提交：

```text
feat: add safe control api snapshot and rollback
```

\---

# 阶段 9：最小 Agent 工具调用闭环

## 62\. 阶段目标

实现最小但完整的 Agent：

```text
用户目标
→ 查询指标
→ 选择工具
→ 保存基线
→ 修改配置
→ 固定时间验证
→ 达标保留 / 不达标回滚
→ 输出报告
```

第一版只支持一个场景：

> 在保证 cam1 推理 FPS 不低于指定值的情况下，降低全局 P95 延迟。

不要一开始同时做故障诊断、Benchmark、VLM 和多 Agent。

## 63\. Agent 模块

```text
agent/
├── main.py
├── goal\_parser.py
├── tool\_registry.py
├── executor.py
├── validator.py
├── rollback.py
├── audit.py
├── schemas.py
└── prompts/
    └── system\_prompt.md
```

## 64\. 工具集

第一版只提供：

```text
get\_system\_metrics()
get\_all\_stream\_status()
get\_scheduler\_state()
set\_infer\_interval(stream\_id, interval)
set\_stream\_priority(stream\_id, priority)
run\_benchmark(duration\_s)
rollback\_config(snapshot\_id)
```

## 65\. Agent 执行策略

### 观察

读取：

* 当前全局 P95；
* 每路 input/inference FPS；
* 每路 priority 和 infer interval；
* GPU/CPU/温度；
* 当前 Scheduler 状态。

### 计划

Agent 只能从有限动作中选择，例如：

```text
保持 cam1 不变
将 cam4 interval 从 0 调为 1
运行 60 秒 Benchmark
```

### 验证

必须比较：

```text
before.p95\_latency\_ms
before.cam1\_inference\_fps
before.drop\_rate

after.p95\_latency\_ms
after.cam1\_inference\_fps
after.drop\_rate
```

### 成功条件示例

```text
after.p95\_latency\_ms <= target
AND after.cam1\_inference\_fps >= 10
AND after.drop\_rate 没有明显恶化
```

### 失败条件

* 未达到目标；
* cam1 FPS 低于约束；
* 温度进入 Critical；
* 丢帧率明显变差；
* API 调用失败；
* Benchmark 数据不完整。

失败必须回滚。

## 66\. Agent System Prompt 核心约束

```text
你是 JetEdge-Agent 的受限运维智能体。
你不能执行 Shell，不能直接修改文件，也不能直接控制 GStreamer 对象。
你只能使用已注册工具。
所有写操作前必须确认当前状态，写操作后必须运行验证。
当目标未达成、关键流约束被破坏、系统进入 Critical 或结果数据不足时，必须调用 rollback\_config。
不得根据主观判断宣布成功，所有结论必须引用工具返回的真实指标。
每轮最多修改两路流，每个流的 infer\_interval 只能是 0..5。
```

## 67\. 给 Claude Code 的提示词

```text
只实现阶段 9：基于已经存在的 Control API，开发一个最小 Tool Calling Agent，完成“降低 P95 延迟，同时保证 cam1 inference FPS 下限”的闭环。

要求：
1. Agent 与视频 Pipeline 分进程；
2. Agent 只能调用 ToolRegistry 中的白名单工具；
3. 不允许 Shell 工具和任意文件写入；
4. 先保存 before 指标；
5. 单轮最多修改 2 路流；
6. 修改后运行固定时长 benchmark；
7. Validator 使用明确的数值条件；
8. 失败必须调用 rollback\_config；
9. 输出 JSON 审计记录和 Markdown 报告；
10. LLM 请求失败时不能影响 Pipeline，也不能留下未验证配置；
11. 为成功、失败、工具超时、指标缺失和回滚失败编写测试；
12. 首先支持一个固定场景，不扩展 VLM、RAG 和多 Agent。

请先设计 Agent 状态机、Tool schema、Validator 条件和失败处理，再实现代码。
```

## 68\. 验收标准

必须录制或保存两次完整运行：

### 成功案例

```text
目标可达
→ Agent 修改低优先级流
→ P95 达标
→ cam1 FPS 满足约束
→ 保留配置
```

### 回滚案例

```text
目标不可达或约束冲突
→ Agent 尝试修改
→ 验证失败
→ 自动回滚
→ 指标恢复
```

Git 提交：

```text
feat: add agent optimization validation and rollback loop
```

\---

# 阶段 10：故障诊断 Agent 与自动 Benchmark

## 69\. 阶段目标

在最小性能优化 Agent 稳定后，再增加两个工具场景。

## 70\. 场景 A：cam3 故障诊断

用户输入：

> cam3 为什么一直掉线？

流程：

```text
get\_stream\_status(cam3)
→ get\_recent\_errors(cam3)
→ 对比其他流状态
→ 判断网络、解码、资源或状态机问题
→ restart\_stream(cam3)
→ 等待恢复窗口
→ 再次查询 input\_fps 和 error\_count
→ 输出诊断报告
```

注意：

* RTSP 自动重连仍由 C++ SourceManager 完成；
* Agent 只是聚合证据、发起一次受控重启并验证；
* 多次失败后必须停止反复重启。

## 71\. 场景 B：自动 Batch Benchmark

用户输入：

> 比较 Batch 1、2、4 的吞吐和 P95 延迟。

流程：

```text
保存原始配置
→ Batch 1，预热 + 测试
→ Batch 2，预热 + 测试
→ Batch 4，预热 + 测试
→ 每组保存环境和指标
→ 恢复原始配置
→ 输出对比表
```

如果运行时无法安全修改 Batch Size，则 Agent 调用外部 Benchmark Runner，由 Runner 依次重启测试进程；不要强行热修改不支持的参数。

## 72\. 验收标准

* 故障诊断报告引用真实错误和状态；
* 单流重启不会影响其他流；
* Benchmark 每组条件一致；
* 最后恢复原配置；
* 输出 CSV、JSON 和 Markdown 报告。

\---

# 阶段 11：稳定性测试、Demo 与项目包装

## 73\. 2 小时稳定性测试

固定记录：

* RSS / RAM；
* 每路 FPS；
* P95/P99；
* GPU 利用率；
* 温度；
* 功耗；
* 错误数；
* 重连次数；
* GStreamer 对象或 request pad 数量；
* Agent 操作次数。

检查：

* 内存是否持续线性增长；
* FPS 是否随时间下降；
* 重连后资源是否释放；
* 指标线程是否卡死；
* Agent API 失败是否影响 Pipeline。

## 74\. Demo 推荐结构

控制在 6～8 分钟。

### 0:00～1:00：问题和架构

* 为什么多路边缘推理比单路难；
* 展示三层架构；
* 强调 Agent 不参与逐帧控制。

### 1:00～2:30：多路推理

* 4 路视频；
* 检测框和 Tracker ID；
* 每路 FPS 和全局 P95；
* 无显示性能模式。

### 2:30～3:30：故障恢复

* 停止 cam3；
* cam1、cam2、cam4 继续；
* cam3 状态变化和自动重连；
* 恢复后重新输出 FPS。

### 3:30～5:30：Agent 性能优化

输入：

> 在保证 cam1 推理 FPS 不低于 10 的情况下，将 P95 延迟降到目标值。

展示：

* Agent 查询指标；
* 调整低优先级流；
* 运行 Benchmark；
* 对比 before/after；
* 达标后保留。

### 5:30～6:30：自动回滚

* 提出不可达目标；
* Agent 验证失败；
* 自动回滚；
* Pipeline 全程持续运行。

### 6:30～7:30：性能分析案例

* 展示 ftrace/trace\_marker；
* 展示 Affinity 或 OSD 优化前后数据；
* 总结瓶颈定位闭环。

## 75\. 最终 README 结构

```text
1. 项目背景与问题
2. 一句话项目故事
3. 系统架构
4. 核心功能
5. 为什么 Agent 不参与实时控制
6. 环境与版本
7. 快速运行
8. 配置说明
9. Metrics 定义
10. Benchmark 方法
11. 实验结果
12. 故障恢复设计
13. Agent 工具、安全与回滚
14. ftrace 性能案例
15. Demo
16. 项目局限与后续工作
```

\---

# 第六部分：项目时间安排

## 76\. 推荐 8 周版本

### 第 1 周：环境与单路 Baseline

* 阶段 0；
* 阶段 1；
* 准备 4 段本地视频；
* 完成 CMake 和基础日志。

成果：

* 单路硬件解码；
* 环境快照；
* 可重复构建。

### 第 2 周：四路 Pipeline 与 FP16

* 阶段 2；
* 阶段 3；
* 无显示模式；
* JSONL Metadata。

成果：

* 4 路检测与追踪；
* FP16 Engine；
* 第一版 Demo。

### 第 3 周：Metrics 与 Benchmark

* 阶段 4；
* B0～B6；
* P50/P95/P99；
* 系统资源采集。

成果：

* 第一版性能报告；
* 明确当前瓶颈。

### 第 4 周：RTSP 与故障恢复

* 阶段 5；
* 构造 4 路 RTSP；
* 故障注入；
* 重连压力测试。

成果：

* 单流故障隔离；
* 自动恢复日志。

### 第 5 周：动态调度

* 阶段 6；
* 固定配置 vs 动态状态机；
* 关键流优先级实验。

成果：

* 一组可写入简历的真实优化数据。

### 第 6 周：Linux 性能分析

* 阶段 7；
* TraceMarker；
* ftrace；
* CPU Affinity 或 OSD 优化案例。

成果：

* 完整问题定位闭环。

### 第 7 周：Control API 与最小 Agent

* 阶段 8；
* 阶段 9；
* 验证与回滚测试。

成果：

* Agent 成功调优；
* Agent 自动回滚。

### 第 8 周：包装与稳定性

* 阶段 10 可选；
* 2 小时稳定性；
* Demo；
* README；
* 简历描述；
* 面试问题。

\---

## 77\. 时间不足时的 4 周压缩版

### 第 1 周

* 单路 → 4 路本地视频；
* TensorRT FP16；
* Tracker；
* 无显示模式。

### 第 2 周

* Metrics；
* P95/P99；
* 4 路 RTSP；
* 单流重连。

### 第 3 周

* 动态 infer interval；
* 流优先级；
* 一组固定 vs 动态数据；
* Control API 和回滚。

### 第 4 周

* 最小 Agent；
* 一次成功调优；
* 一次失败回滚；
* 2 小时稳定性；
* Demo 和 README。

压缩版可以删除：

* INT8；
* Grafana；
* 自动 Batch Benchmark；
* VLM；
* Docker；
* 复杂 ftrace 分析。

但建议至少保留一次简单的 `trace\_marker + sched` 分析截图或报告。

\---

# 第七部分：实验设计与数据可信度

## 78\. 必做实验

### 实验 1：视频路数扩展

```text
1 路、2 路、4 路
```

记录：

* 总吞吐；
* 每路推理 FPS；
* P95/P99；
* GPU；
* RAM；
* 温度和功耗。

### 实验 2：Batch Size

```text
Batch 1、2、4
```

回答：

> Batch 增大带来的吞吐提升，是否值得额外 Batch 等待和尾延迟？

### 实验 3：Infer Interval

```text
interval 0、1、2、3
```

回答：

> 降低模型调用频率后，Tracker 能否保持输出稳定？吞吐、延迟和检测效果如何变化？

### 实验 4：固定配置 vs 动态调度

记录：

* cam1 推理 FPS；
* 全局 P95；
* 丢帧；
* 温度；
* 调度动作次数。

### 实验 5：RTSP 故障

记录：

* 故障发现时间；
* 重连次数；
* 恢复时间；
* 其他流 FPS 波动；
* 是否资源泄漏。

### 实验 6：Agent 闭环

对比：

```text
人工固定配置
vs
C++ 动态状态机
vs
Agent 目标驱动调优
```

Agent 的价值不是一定比状态机性能更高，而是：

* 能理解不同目标和约束；
* 能组合已有工具；
* 能自动运行实验；
* 能解释结果；
* 能失败回滚。

## 79\. 实验公平性规则

* 使用相同视频源；
* 使用相同模型和 Engine；
* 固定功耗模式；
* 固定预热时间；
* 固定测量窗口；
* 不将预热数据纳入结果；
* 每组至少运行 3 次；
* 报告均值和波动；
* 保存完整配置和环境快照；
* 不只截图峰值数据。

\---

# 第八部分：面试表达模板

## 80\. 30 秒项目介绍

> 我在 Jetson Orin Nano 上搭建了一套 4 路视频边缘推理和智能运维平台。底层使用 C++、GStreamer/DeepStream 和 TensorRT 完成硬件解码、Batch 推理、目标追踪和事件统计；中间控制层采集每路 FPS、P95/P99 延迟、Batch 等待、温度和功耗，并根据负载动态调整低优先级流的推理间隔，同时支持 RTSP 单流故障隔离和恢复；上层 Agent 只通过白名单 Control API 查询和修改系统，能够执行调优、运行 Benchmark、验证效果，并在目标未达成时自动回滚。

## 81\. 2 分钟项目主线

### 背景

> 我之前在树莓派上做过单路跌倒检测，已经验证了低算力 CPU 上的应用闭环。新项目希望进一步证明我能处理 GPU 异构、多路视频和系统性能问题，因此没有继续做单一视觉应用，而是设计了一个多路推理平台。

### 难点

> 真正困难的不是模型能不能跑，而是四路视频同时接入后，Batch 等待、解码线程、推理负载、网络抖动和温度会共同影响尾延迟，而且单流断开不能拖垮其他流。

### 方案

> 我把系统拆成数据面、确定性控制面和 Agent 面。数据面负责实时处理，控制面负责状态机调度、故障恢复和回滚，Agent 只做秒级目标驱动决策，并通过白名单工具操作系统。

### 证据

> 我建立了 B0 到 B6 的分层 Benchmark，并采集每路 FPS、P95/P99、Batch 填充率、GPU、温度和功耗。然后通过动态 infer interval 和流优先级保障关键流；另外使用 trace\_marker 和 ftrace 定位了一次真实线程调度或 OSD 开销问题。

### Agent 价值

> Agent 不只是给建议，它必须查询真实指标、保存修改前快照、调用控制工具、运行验证，不达标就自动回滚。这样 Agent 即使判断错误，也不会破坏实时 Pipeline。

\---

## 82\. 面试官可能追问

### 为什么不用纯 OpenCV？

回答重点：

* DeepStream 直接利用 Jetson 硬件解码和 NVIDIA Metadata；
* 适合多流 Batch；
* 减少 CPU-GPU 拷贝；
* 自带 Tracker、OSD 和插件化 Pipeline；
* 但复杂动态 Source 管理需要正确处理 GStreamer 生命周期。

### 为什么 Agent 不直接控制每一帧？

回答重点：

* LLM 延迟和输出不确定；
* 实时控制必须由确定性状态机完成；
* Agent 只负责秒级目标、实验和解释；
* Critical 状态下控制面优先级高于 Agent。

### Agent 和规则调度有什么区别？

回答重点：

* 状态机适合已知、低延迟和安全规则；
* Agent 适合组合多个工具、理解业务约束、自动设计 Benchmark；
* Agent 不替代状态机，而是调用和配置状态机；
* Agent 的成功必须由数值 Validator 判断。

### 如何证明优化有效？

回答重点：

* 同一输入和功耗模式；
* 固定预热与测试时长；
* 记录 P95/P99、关键流 FPS、丢帧、温度和功耗；
* 至少多次运行；
* 不达标回滚；
* 保存完整 before/after 配置。

### RTSP 重连为什么难？

回答重点：

* 动态移除 Source Bin；
* Request Pad 生命周期；
* GstBus 错误定位到具体流；
* 重连不能阻塞主循环；
* 旧对象和队列必须正确释放；
* 慢流和故障流不能拖累 Batch。

\---

# 第九部分：最终简历描述模板

## 83\. 项目描述

**JetEdge-Agent：基于 Jetson 的多路视频边缘 AI 推理与智能运维平台**

面向多摄像头边缘感知场景，基于 Jetson Orin Nano、C++、GStreamer/DeepStream 和 TensorRT 构建多路视频实时推理平台，实现硬件解码、批量推理、目标追踪、性能观测、动态资源调度、RTSP 故障恢复，以及 Agent 驱动的调优、验证与自动回滚。

## 84\. 简历要点模板

* 基于 C++ 与 GStreamer/DeepStream 搭建 `\[实测路数]` 路 `\[分辨率/FPS]` 视频处理管线，完成硬件解码、`nvstreammux` Batch、TensorRT FP16 推理、目标追踪和结构化结果输出。
* 构建覆盖每路输入/推理 FPS、P50/P95/P99 延迟、Batch 等待与填充率、CPU/GPU、内存、温度、功耗及丢帧率的可观测体系，并设计 B0～B6 分层 Benchmark 定位性能瓶颈。
* 实现 RTSP 单流状态机和指数退避重连机制，使单路断流不影响其他视频流，并完成 `\[故障次数]` 次故障注入及 `\[测试时长]` 稳定性测试。
* 设计基于负载、温度、队列与流优先级的 C++ 动态调度器，通过调整低优先级流推理间隔，在保证关键流 `\[实际 FPS]` 的条件下将 `\[P95/丢帧/温度/功耗]` 从 `\[优化前]` 优化至 `\[优化后]`。
* 使用 `trace\_marker`、ftrace 和 CPU Affinity 分析视频处理线程调度，定位 `\[真实瓶颈]`，并通过 `\[真实优化方法]` 改善 `\[实际指标]`。
* 将 Metrics、Stream Manager、Scheduler 和 Benchmark 封装为白名单 Control API，设计配置快照、参数校验、审计日志与回滚机制。
* 实现 Tool Calling Agent，根据自然语言目标查询真实指标、修改运行参数、执行 Benchmark 并自动验证；目标未达成或关键流约束被破坏时自动回滚，完成性能优化与故障诊断演示。

注意：所有数字必须替换为实测值，未完成的功能不要写入简历。

\---

# 第十部分：每次给 Claude Code 的通用提示模板

## 85\. 新功能模板

```text
当前阶段：<阶段名称>

当前仓库状态：
- 已完成功能：<列出>
- 当前可运行命令：<命令>
- 当前已知问题：<列出>
- 环境版本：见 docs/environment\_snapshot.md

本次只完成：
<一个清晰、可验收的功能>

明确不做：
<列出后续功能，防止越界>

要求：
1. 先只读分析相关代码；
2. 给出设计、修改文件和风险；
3. 采用最小增量修改；
4. 不改变已稳定接口，除非说明原因；
5. 添加必要测试；
6. 给出构建和运行命令；
7. 给出验收步骤；
8. 不安装依赖，不执行 sudo apt update；
9. 不猜测 Jetson/DeepStream API，优先检查本机样例和头文件；
10. 完成后总结实际修改、假设和未解决问题。
```

## 86\. 报错修复模板

```text
下面是 Jetson 实机上的完整报错，请基于当前仓库定位，不要先大范围重构。

执行命令：
<命令>

完整输出：
<粘贴日志>

预期行为：
<预期>

实际行为：
<实际>

要求：
1. 先指出最可能的根因链；
2. 区分编译问题、链接问题、插件问题、配置问题和运行时状态问题；
3. 只修改与根因直接相关的代码；
4. 增加能提前暴露该问题的错误检查或日志；
5. 给出复现和验证命令；
6. 不通过删除功能或吞掉异常来“修复”。
```

## 87\. 性能分析模板

```text
当前性能问题：
<例如 4 路时 P95 延迟升高>

测试条件：
- 视频：<信息>
- 路数：<信息>
- 模型/Engine：<信息>
- Batch：<信息>
- infer\_interval：<信息>
- 功耗模式：<信息>
- 是否 OSD：<信息>

指标：
<粘贴 JSON/CSV 摘要>

要求：
1. 不直接提出优化代码；
2. 先根据指标列出 2～4 个假设；
3. 为每个假设设计最小验证实验；
4. 判断需要增加哪些埋点；
5. 一次只验证一个变量；
6. 证据不足时明确说明，不猜结论。
```

## 88\. 代码审查模板

```text
请审查当前阶段的实现，重点不是代码风格，而是 Jetson 多路实时系统的正确性。

重点检查：
1. GStreamer 对象和 request pad 生命周期；
2. 动态 pad 连接；
3. 错误路径资源释放；
4. 多线程数据竞争；
5. Pipeline 主线程是否被阻塞；
6. Metrics 热路径开销；
7. 状态机边界条件；
8. 配置修改是否可回滚；
9. 日志是否足以定位 stream\_id 和阶段；
10. 是否存在 Agent 绕过 Control API 的路径。

请按严重程度输出：
- 必须修复；
- 建议修复；
- 可后续优化。

对于必须修复项，给出最小补丁方案和验证方法。
```

\---

# 第十一部分：项目验收清单

## 89\. 数据面

* \[ ] 单路本地视频硬件解码；
* \[ ] 4 路本地视频 Batch；
* \[ ] TensorRT FP16；
* \[ ] Tracker；
* \[ ] JSONL Metadata；
* \[ ] 无显示模式；
* \[ ] 4 路 RTSP；
* \[ ] 单流断流隔离；
* \[ ] 自动重连。

## 90\. 可观测性

* \[ ] 每路输入 FPS；
* \[ ] 每路推理 FPS；
* \[ ] 全局和每路 P95/P99；
* \[ ] Batch 等待；
* \[ ] Batch 填充率；
* \[ ] 丢帧；
* \[ ] CPU/GPU/RAM；
* \[ ] 温度和功耗；
* \[ ] B0～B6 Benchmark。

## 91\. 控制与优化

* \[ ] 动态 infer interval；
* \[ ] 流优先级；
* \[ ] 滞回和冷却；
* \[ ] 关键流保护；
* \[ ] 固定 vs 动态实验；
* \[ ] TraceMarker；
* \[ ] 一条 ftrace/Affinity 案例。

## 92\. Agent

* \[ ] 白名单 Tool Registry；
* \[ ] 参数限制；
* \[ ] 配置快照；
* \[ ] 审计日志；
* \[ ] before/after 指标；
* \[ ] Validator；
* \[ ] 成功保留；
* \[ ] 失败回滚；
* \[ ] LLM 不可用时不影响 Pipeline；
* \[ ] 一次性能优化 Demo；
* \[ ] 一次故障诊断 Demo。

## 93\. 项目包装

* \[ ] 2 小时稳定性报告；
* \[ ] 架构图；
* \[ ] Benchmark 图表；
* \[ ] 故障恢复时序图；
* \[ ] Agent 执行时序图；
* \[ ] Demo 视频；
* \[ ] README；
* \[ ] 简历描述；
* \[ ] 面试问答。

\---

# 第十二部分：最终实施原则

## 94\. 永远遵循的开发顺序

```text
先让单路正确运行
→ 再扩展四路
→ 再接 TensorRT
→ 再建立指标
→ 再做 RTSP 恢复
→ 再做动态调度
→ 再做性能 Trace
→ 再封装 Control API
→ 最后开发 Agent
```

## 95\. 每一项优化都必须形成闭环

```text
现象
→ 指标证据
→ 假设
→ 单变量实验
→ 修改
→ before/after
→ 成功保留或失败回滚
```

## 96\. 项目最终最有价值的一句话

> 该项目不是将多个技术名词堆在 Jetson 上，而是通过一套可复现的工程流程，使多路视频系统能够稳定运行、准确测量、定位瓶颈、动态调度，并允许 Agent 在安全边界内执行真实操作，用数据证明有效或自动回滚。

