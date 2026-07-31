# JetEdge-Agent

基于 Jetson Orin Nano 的多路视频边缘 AI 推理与智能运维平台。

> 项目当前处于开发阶段。文档中的性能指标、视频路数、延迟、功耗和优化比例，均以 Jetson 实机测试结果为准。

---

## 1. 项目简介

JetEdge-Agent 面向园区、交通路口、工厂和自动驾驶测试场等多摄像头边缘感知场景，目标是在 Jetson Orin Nano 8GB 上构建一套：

- 多路视频接入与硬件解码；
- GStreamer / DeepStream 多流处理；
- TensorRT FP16 / INT8 推理；
- 目标检测、追踪和事件统计；
- FPS、延迟、温度、功耗和丢帧监控；
- 基于负载和业务优先级的动态调度；
- RTSP 断流检测、故障隔离和自动恢复；
- ftrace、trace_marker 和 CPU Affinity 性能分析；
- Agent 查询、调优、验证和自动回滚；

的一体化边缘 AI 平台。

项目重点不是“在 Jetson 上运行一个 YOLO 模型”，而是：

> 在有限功耗、内存和算力约束下，构建一套多路、可观测、可调度、可恢复、可分析，并能够被 Agent 安全控制的实时边缘推理系统。

---

## 2. 项目背景

此前已完成一个基于树莓派的边缘端实时跌倒检测系统，主要关注：

- CPU 低算力部署；
- 单路或少路视频；
- YOLOv8n + MoveNet；
- ONNX Runtime；
- OpenCV 多线程 Pipeline；
- ByteTrack；
- MJPEG Web；
- SQLite 和告警闭环。

JetEdge-Agent 在此基础上进一步验证：

- GPU 异构计算；
- 多路视频处理；
- 硬件解码；
- TensorRT 部署；
- DeepStream Batch；
- 系统性能观测；
- 动态资源调度；
- Linux 调度分析；
- Agent 工具调用和回滚。

两个项目形成的能力成长路线为：

```text
树莓派低算力边缘 AI 应用闭环
            ↓
Jetson 多路 GPU 异构推理与智能运维平台
```

---

## 3. 系统架构

```text
多路本地视频 / RTSP / 摄像头
                │
                ▼
           Source Manager
   解复用、状态管理、断流检测、自动重连
                │
                ▼
       nvv4l2decoder 硬件解码
                │
                ▼
         nvstreammux 多流 Batch
                │
                ▼
        GPU 预处理 / TensorRT 推理
                │
                ▼
         检测后处理 / Tracker
                │
                ▼
       ROI / 越线 / 事件统计 / JSON
                │
       ┌────────┼─────────┐
       ▼        ▼         ▼
    可视化    Metrics    Event Store

控制面：
Metrics Collector
        ↓
Adaptive Scheduler
        ↓
Control Server
        ↓
推理间隔、流优先级、单流状态和运行策略

Agent 面：
自然语言目标
        ↓
查询真实指标
        ↓
调用白名单工具
        ↓
执行有限范围修改
        ↓
重新 Benchmark
        ↓
保留配置或自动回滚
```

---

## 4. 三层职责

### 4.1 数据面

负责实时视频处理：

- 视频接入；
- 硬件解码；
- 多路 Batch；
- TensorRT 推理；
- 目标追踪；
- 事件统计；
- 结构化结果输出。

### 4.2 确定性控制面

负责低延迟、可预测的运行时控制：

- 队列和丢帧管理；
- RTSP 自动重连；
- 推理间隔调整；
- 流优先级；
- 温度和负载状态机；
- 配置快照；
- 故障恢复；
- 回滚。

### 4.3 Agent 智能决策面

负责秒级或分钟级分析：

- 查询系统状态；
- 分析指标和错误；
- 选择控制工具；
- 执行 Benchmark；
- 对比优化前后结果；
- 判断是否达到目标；
- 不达标时回滚；
- 生成性能或故障报告。

Agent 不参与逐帧决策，也不直接操作 DeepStream 内部对象。

---

## 5. 核心技术栈

### 硬件

- Jetson Orin Nano 8GB；
- NVMe SSD；
- 主动散热；
- 稳定电源；
- 千兆网络。

### 软件

- Ubuntu 22.04；
- JetPack 6 系列；
- CUDA 12 系列；
- TensorRT 10 系列；
- DeepStream 7 系列；
- GStreamer；
- C++17；
- CMake；
- Python 3；
- Prometheus；
- Grafana；
- Docker；
- 可选远程 LLM API。

> 实际版本以 `docs/environment_report.md` 中的 Jetson 实机核查结果为准，不在 README 中提前假设。

---

## 6. 当前开发状态

### 已完成

- [x] Jetson Orin Nano 远程开发环境配置；
- [x] VS Code Remote-SSH 连接；
- [x] Claude Code 配置；
- [x] 项目总体架构设计；
- [x] 分阶段实施方案；
- [x] Claude Code 仓库级开发规则；
- [x] **阶段 0**：只读环境核查与 C++17/CMake 工程骨架；
- [x] **阶段 1**：单路本地视频硬件解码基线（`.h264` 裸流 + `.mp4` 容器）。

### 当前阶段

- [ ] **阶段 3**：TensorRT FP16 检测、Tracker 与结构化结果（需要准备模型文件）。

### 后续阶段

- [ ] 四路本地视频与 `nvstreammux`；
- [ ] TensorRT FP16 推理；
- [ ] 目标追踪和结构化输出；
- [ ] Metrics 和分层 Benchmark；
- [ ] RTSP 断流恢复；
- [ ] C++ 动态调度器；
- [ ] ftrace 和 CPU Affinity 分析；
- [ ] Control API、配置快照和回滚；
- [ ] 最小 Agent 工具调用闭环；
- [ ] 性能优化和故障诊断 Agent；
- [ ] 稳定性测试、Demo 和项目包装。

---

## 7. 推荐开发顺序

```text
环境核查
    ↓
单路本地视频硬件解码
    ↓
四路视频与 nvstreammux
    ↓
TensorRT FP16 推理
    ↓
Tracker 和结构化输出
    ↓
Metrics 与 Benchmark
    ↓
RTSP 故障恢复
    ↓
动态调度
    ↓
ftrace 和 CPU Affinity
    ↓
Control API 与回滚
    ↓
Agent 工具调用
    ↓
验证、保留或自动回滚
```

不要从以下内容开始：

- 多 Agent；
- 大型 RAG；
- 本地大参数模型；
- Linux 内核模块；
- 自定义 CUDA 全量后处理；
- 跨摄像头 ReID；
- 复杂多任务模型。

---

## 8. 目标功能

### P0：必须完成

- C++ 多路视频接入；
- GStreamer / DeepStream Pipeline；
- Jetson 硬件解码；
- TensorRT FP16 推理；
- 多路 Batch；
- 目标追踪；
- 结构化 JSON 输出；
- RTSP 断流恢复；
- FPS、P50 / P95 延迟和丢帧统计；
- 无渲染 Benchmark；
- 至少 2 小时稳定性测试；
- Metrics API；
- Control API；
- 最小 Agent 工具调用闭环；
- Agent 验证与自动回滚。

### P1：核心亮点

- TensorRT INT8 PTQ；
- Prometheus / Grafana；
- 动态推理间隔；
- 流优先级；
- Batch Size / Timeout 对比；
- ftrace、trace_marker；
- CPU Affinity；
- Agent 性能优化；
- Agent 故障诊断；
- 自动 Benchmark；
- Docker 部署。

### P2：选做增强

- VLM 关键帧或短视频复核；
- CUDA 自定义后处理；
- 自定义 GStreamer 插件；
- 多任务模型；
- Linux 内核模块；
- 跨摄像头 ReID。

---

## 9. 项目目录

项目初期只创建当前阶段需要的目录，避免一次生成空壳。

推荐结构：

```text
jetedge-agent/
├── CLAUDE.md
├── README.md
├── CMakeLists.txt
├── apps/
│   ├── jetedge_server/
│   ├── benchmark/
│   ├── control_cli/
│   └── agent_demo/
├── include/jetedge/
│   ├── pipeline/
│   ├── inference/
│   ├── tracking/
│   ├── analytics/
│   ├── scheduler/
│   ├── metrics/
│   ├── control/
│   └── common/
├── src/
│   ├── pipeline/
│   ├── inference/
│   ├── tracking/
│   ├── analytics/
│   ├── scheduler/
│   ├── metrics/
│   └── control/
├── agent/
├── configs/
├── models/
├── monitoring/
├── scripts/
├── tests/
└── docs/
    ├── implementation_plan.md
    ├── environment_report.md
    ├── architecture.md
    ├── benchmark.md
    ├── scheduler.md
    ├── agent_design.md
    ├── tool_api.md
    ├── safety.md
    └── demo.md
```

---

## 10. 环境核查

项目开始前仅执行只读核查，不执行 `sudo apt update`：

```bash
echo "===== OS ====="
cat /etc/os-release
uname -a

echo "===== Jetson Linux ====="
head -n 1 /etc/nv_tegra_release 2>/dev/null || true

echo "===== JetPack ====="
dpkg-query -W nvidia-jetpack 2>/dev/null || true

echo "===== CUDA ====="
nvcc --version 2>/dev/null || true

echo "===== TensorRT ====="
trtexec --version 2>/dev/null || /usr/src/tensorrt/bin/trtexec --version 2>/dev/null || true

echo "===== DeepStream ====="
deepstream-app --version-all 2>/dev/null || true

echo "===== GStreamer ====="
gst-inspect-1.0 --version
gst-inspect-1.0 nvv4l2decoder 2>/dev/null | head -n 20

echo "===== Power Mode ====="
nvpmodel -q --verbose 2>/dev/null || true

echo "===== Hardware ====="
cat /proc/device-tree/model 2>/dev/null
```

核查结果写入：

```text
docs/environment_report.md
```

---

## 11. 测试视频

第一阶段优先使用 Jetson 自带 DeepStream 样例视频：

```text
/opt/nvidia/deepstream/deepstream/samples/streams/
```

常用输入：

```text
sample_1080p_h264.mp4
```

开发顺序：

```text
单路官方样例
    ↓
同一视频重复四路
    ↓
四段不同场景的 H.264 视频
    ↓
四路 RTSP
    ↓
抖动、丢包和断流测试
```

---

## 12. 构建方式

当前构建方式：

```bash
cmake -S . -B build
cmake --build build -j2
```

运行命令（YAML 配置文件，支持 1/2/4 路）：

```bash
# 编辑 configs/streams.yaml 调整路数，然后运行：
./build/jetedge_server configs/streams.yaml
```

---

## 13. Benchmark 设计

分层 Benchmark：

```text
B0：解码 → fakesink
B1：解码 → nvstreammux → fakesink
B2：解码 → preprocess → fakesink
B3：解码 → inference → fakesink
B4：解码 → inference → tracker → fakesink
B5：完整 Pipeline，无 OSD
B6：完整 Pipeline + OSD
B7：完整 Pipeline + Metrics
B8：完整 Pipeline + RTSP 故障恢复
B9：完整 Pipeline + Agent 查询
B10：完整 Pipeline + Agent 调优和回滚
```

每组记录：

- 输入 FPS；
- 解码 FPS；
- 推理 FPS；
- 输出 FPS；
- P50 / P95 / P99 延迟；
- Batch 等待时间；
- Batch 填充率；
- 丢帧率；
- CPU、GPU 和 RAM；
- 温度；
- 功耗；
- 重连次数；
- Agent 控制耗时。

---

## 14. 动态调度

调度器采用确定性的 C++ 状态机：

| 状态 | 运行策略 |
|---|---|
| Normal | 正常分辨率和推理频率 |
| Pressure | 增大推理间隔，使用追踪补偿 |
| Thermal | 降低低优先级流推理频率 |
| Critical | 暂停低优先级流推理 |
| Recovery | 负载恢复后逐级恢复配置 |

调度器必须包含：

- 进入阈值；
- 退出阈值；
- 滞回；
- 最小保持时间；
- 冷却时间；
- 最大调整频率。

---

## 15. Agent 安全边界

Agent 只能调用白名单工具：

```text
get_system_metrics
get_stream_status
get_all_stream_status
get_scheduler_config
get_recent_errors
set_stream_priority
set_infer_interval
restart_stream
run_benchmark
rollback_config
```

所有写操作必须：

- 参数校验；
- 权限校验；
- 范围限制；
- 修改前保存快照；
- 执行超时；
- 审计日志；
- 修改后重新验证；
- 不达标自动回滚。

Agent 禁止：

- 执行任意 Shell；
- 任意读写系统文件；
- 绕过 C++ 状态机；
- 关闭全部关键流；
- 在过热状态下增加负载；
- 未经验证宣布优化成功。

---

## 16. 预期 Demo

### Demo 1：四路视频基础能力

- 四路视频接入；
- 检测框和追踪 ID；
- 每路 FPS；
- 系统指标；
- 单路 RTSP 断开；
- 其他流持续运行；
- 自动重连恢复。

### Demo 2：性能优化 Agent

用户输入：

```text
在保证 cam1 推理频率不低于 10 FPS 的情况下，
降低当前 Pipeline P95 延迟。
```

Agent 流程：

```text
查询指标
→ 保存配置快照
→ 修改低优先级流
→ 运行 Benchmark
→ 对比前后数据
→ 保留或回滚
```

### Demo 3：自动回滚

- 人为设置无法达到的目标；
- Agent 执行修改；
- 验证失败；
- 自动回滚；
- 实时视频 Pipeline 不受影响。

### Demo 4：故障诊断

用户输入：

```text
cam3 为什么一直掉线？
```

Agent 查询错误、执行单流恢复并验证结果。

---

## 17. 验收标准

### 基础能力

- 4 路视频稳定接入；
- 硬件解码正常；
- 检测、追踪和事件统计正常；
- 单流故障不影响其他流；
- RTSP 恢复后自动重连；
- 连续运行至少 2 小时；
- 无明显内存持续增长。

### 性能能力

- 输出每路 FPS；
- 输出 P50 / P95 / P99 延迟；
- 输出 Batch 等待和填充率；
- 输出 CPU、GPU、RAM、温度和功耗；
- 完成 FP16 / INT8 对比；
- 完成 Batch Size / Timeout 对比；
- 完成无渲染和 OSD 对比。

### Agent 能力

- 查询真实指标；
- 调用真实控制工具；
- 完成配置修改；
- 修改后重新验证；
- 失败时自动回滚；
- 操作有审计日志；
- Agent 故障不影响底层 Pipeline；
- 报告包含实测数据。

---

## 18. 开发约束

仓库级开发规则见：

```text
CLAUDE.md
```

关键约束：

- 不执行或建议执行 `sudo apt update`；
- 未经明确许可不安装系统软件；
- DeepStream、TensorRT 和 GStreamer API 必须通过 Jetson 实机确认；
- 一次只实现一个阶段；
- 不覆盖未提交代码；
- 不打印或提交 API Key；
- 每次修改必须给出构建、运行和验收方式；
- 未在 Jetson 上实际验证的功能不得标记为完成。

---

## 19. 文档索引

| 文档 | 说明 |
|---|---|
| `CLAUDE.md` | Claude Code 仓库级开发规则 |
| `docs/implementation_plan.md` | 分阶段实施方案 |
| `docs/environment_report.md` | Jetson 实机环境核查 |
| `docs/architecture.md` | 系统架构设计 |
| `docs/benchmark.md` | Benchmark 方法与结果 |
| `docs/scheduler.md` | 动态调度器设计 |
| `docs/agent_design.md` | Agent 状态机和工具设计 |
| `docs/tool_api.md` | Control API 和工具协议 |
| `docs/safety.md` | Agent 权限、验证和回滚 |
| `docs/demo.md` | Demo 流程和演示脚本 |

---

## 20. 项目成果目标

最终计划输出：

- C++ DeepStream 多路推理程序；
- TensorRT FP16 / INT8 Engine；
- 多路 RTSP 测试环境；
- Tracker 和事件统计；
- RTSP 故障恢复模块；
- Metrics Exporter；
- Grafana Dashboard；
- 动态调度器；
- Control Server；
- Agent Tool Registry；
- 验证与自动回滚；
- Agent 审计日志；
- Benchmark 脚本；
- ftrace 性能分析报告；
- CPU Affinity 优化结果；
- 稳定性测试报告；
- Demo 视频；
- 完整项目文档；
- 简历项目描述。

---

## 21. License

项目许可证将在代码公开前确定。
