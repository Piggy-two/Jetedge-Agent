# JetEdge-Agent

## GitHub 同步快照（2026-08-01）

- 阶段 3 已完成并验收通过：YOLO11s ONNX 输入 `1x3x384x640`，输出 `1x84x5040`。
- ONNX Checker、ONNX Runtime inference、NaN/Inf check 均为 `PASSED`。
- Windows 与 Jetson SHA256 已确认一致：`41abd2ff906712b41c60de9b7d5d5f09918e23a331d80cc0926071600fd3e078`。
- Jetson 模型路径：`/home/seeed/JetEdge-Agent/models/yolo11s.onnx`。
- **阶段 4 已完成并验收通过（2026-08-01）**：Jetson 上构建 TensorRT FP16 Engine 成功（`yolo11s_b1_384x640_fp16.engine`，21.81 MiB，SHA256 `c6cc41d0...a82274a`），自写 YOLO11 自定义 parser 接入单路 DeepStream `nvinfer`，单路 720p 视频 1440 帧检测正常（bus/car 高置信，与 ground truth 吻合），EOS / Ctrl-C / 内存行为全部验证通过。
- 当前准备进入阶段 5：四路检测、Tracker、结构化输出和 Metrics。
- GitHub 同步源码、配置模板、脚本、Markdown 和 `models/model_info.txt`；模型、视频、Engine、密钥和大日志通过 `.gitignore` 排除。
- 模型和其他大文件通过 SCP/rsync 同步，并用 SHA256 做 Windows 与 Jetson 端到端一致性验收。
- 大模型策略：事件驱动、按需调用、异步处理；Kimi/DeepSeek/Agent 不进入实时逐帧主链路。

基于 Jetson Orin Nano 8GB 的多路视频边缘 AI 推理、多模态事件理解与安全智能运维平台。

> 项目处于分阶段开发状态。README 中只有经过实机验收的内容才标记为完成；性能、延迟、功耗、吞吐量和优化比例均以 Jetson 实测结果为准。

## 当前状态快照

- 当前日期：2026-08-01
- 已完成：Jetson 环境与远程开发基础、单路本地视频硬件解码、YOLO11s ONNX 导出验证、模型传输与 SHA256 一致性验收、**阶段 4（TensorRT FP16 Engine + 单路 nvinfer 检测验证）**
- 当前阶段：**阶段 5——四路检测、Tracker、结构化输出和 Metrics**
- 当前禁止提前开展：RTSP、事件系统、Kimi、DeepSeek、Agent 工具执行、INT8

当前模型信息：

```text
模型：YOLO11s
输入：1x3x384x640 FP32
输出：1x84x5040
Batch：1
Dynamic Shape：false
ONNX Checker：PASSED
ONNX Runtime inference：PASSED
NaN/Inf check：PASSED
SHA256：41abd2ff906712b41c60de9b7d5d5f09918e23a331d80cc0926071600fd3e078
Jetson 路径：/home/seeed/JetEdge-Agent/models/yolo11s.onnx
```

---

## 1. 项目简介

JetEdge-Agent 面向园区、交通路口、工厂和自动驾驶测试场等多摄像头边缘感知场景，目标是在 Jetson Orin Nano 8GB 上构建一套：

- 多路视频接入与硬件解码；
- GStreamer / DeepStream 多流处理；
- YOLO11s TensorRT FP16 / INT8 推理；
- 目标检测、追踪和事件统计；
- FPS、延迟、温度、功耗和丢帧监控；
- 基于负载和业务优先级的确定性动态调度；
- RTSP 断流检测、故障隔离和自动恢复；
- ftrace、trace_marker 和 CPU Affinity 性能分析；
- 低频事件触发的 Kimi 多模态复核；
- DeepSeek 文本诊断、日志归因和候选操作规划；
- Agent 白名单工具调用、验证、审计和自动回滚。

项目重点不是“在 Jetson 上运行一个 YOLO 模型”，而是：

> 在有限功耗、内存和算力约束下，构建一套多路、可观测、可调度、可恢复、可分析，并能够被 Agent 安全控制的实时边缘推理系统。

核心原则：

```text
实时主链路必须独立于云端大模型运行。
本地规则能够确定的事件，不调用大模型。
视觉语义不确定时调用 Kimi。
系统指标、日志和运维规划调用 DeepSeek。
模型只产生分析或候选计划，本地策略模块负责最终执行权限。
```

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
- 多模态事件复核；
- Agent 工具调用、验证和回滚。

能力成长路线：

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
       YOLO11s TensorRT / nvinfer
                │
                ▼
         检测后处理 / Tracker
                │
                ▼
       ROI / 越线 / 停留 / 系统事件
                │
      ┌─────────┼──────────────┐
      ▼         ▼              ▼
  可视化     Metrics       Event Store
                │              │
                │              ▼
                │      事件去重 / 合并 / 分级
                │              │
                │      ┌───────┴────────┐
                │      ▼                ▼
                │  Kimi 多模态      DeepSeek 文本
                │  视觉事件复核      指标与日志诊断
                │      └───────┬────────┘
                │              ▼
                │       Unified Analysis
                │              │
                ▼              ▼
       Adaptive Scheduler   Agent Policy
                │              │
                └──────┬───────┘
                       ▼
              Safe Control Executor
                       │
                       ▼
               验证 / 保留 / 回滚
```

大模型调用不进入逐帧主链路：

```text
实时链路：YOLO → Tracker → Event Rule → 立即告警
智能增强：事件路由 → 去重聚合 → Kimi 或 DeepSeek → 更新结论
自动运维：异常 → DeepSeek 候选计划 → Policy 校验 → 执行 → 验证
```

---

## 4. 分层职责

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

### 4.3 智能分析面

负责秒级或分钟级分析：

- Kimi 对关键帧或短事件视频进行低频视觉复核；
- DeepSeek 分析指标、错误摘要和日志；
- 生成事件摘要、故障原因和候选操作计划；
- 默认异步运行；
- API 失败不影响视频 Pipeline。

### 4.4 Agent 安全执行面

负责把候选计划转换为受控操作：

- 只允许调用白名单工具；
- 校验参数类型和范围；
- 检查当前安全状态；
- 修改前保存配置快照；
- 执行后采集指标并验证；
- 不达标自动回滚；
- 写入完整审计日志。

Agent 不参与逐帧决策，也不直接操作 DeepStream 内部对象。

---

## 5. 核心技术栈

### 硬件

- Jetson Orin Nano 8GB；
- NVMe SSD；
- 主动散热；
- 稳定电源；
- 千兆网络。

### Jetson 软件

- Ubuntu 22.04；
- JetPack、CUDA、TensorRT、DeepStream：以 `docs/environment_report.md` 的实机核查结果为准；
- GStreamer；
- C++17；
- CMake；
- Python 3；
- 可选 Prometheus / Grafana；
- 可选 Docker。

### 模型和云端能力

- YOLO11s：实时目标检测；
- Tracker：非检测帧补偿和时序事件；
- Kimi：低频多模态事件理解；
- DeepSeek：文本诊断、指标分析和候选操作规划；
- 本地 Agent：策略校验、工具执行、结果验证和回滚。

> 不在 README 中提前假设 JetPack、CUDA、TensorRT、DeepStream 的具体版本。所有版本结论来自当前 Jetson 实机。

---

## 6. 当前开发状态

### 已完成

- [x] Jetson Orin Nano 远程开发环境配置；
- [x] VS Code Remote-SSH 连接；
- [x] Claude Code 配置；
- [x] 项目总体架构设计；
- [x] 分阶段实施方案；
- [x] Claude Code 仓库级开发规则；
- [x] 阶段 0：只读环境核查与 C++17/CMake 工程骨架；
- [x] 阶段 1：单路本地视频硬件解码基线（`.h264` 裸流 + `.mp4` 容器）；
- [x] 阶段 3：Windows 主机导出 YOLO11s ONNX；
- [x] ONNX Checker、ONNX Runtime 和 NaN/Inf 验证；
- [x] 生成 `models/model_info.txt`；
- [x] ONNX 和 `model_info.txt` 传输到 Jetson；
- [x] Windows 与 Jetson SHA256 一致性验收。
- [x] 阶段 4：Jetson 上构建 TensorRT FP16 Engine（`yolo11s_b1_384x640_fp16.engine`，21.81 MiB，SHA256 `c6cc41d0...a82274a`）；
- [x] 阶段 4：自写 YOLO11 自定义 parser（`src/inference/yolo11_parser.cpp`）并接入单路 DeepStream `nvinfer`；
- [x] 阶段 4：单路 720p 视频验证——1440 帧检测正常（bus/car 高置信，与 ground truth 吻合）、EOS / Ctrl-C / 内存行为通过。

### 当前阶段

- [ ] **阶段 5：四路检测、Tracker、结构化输出和 Metrics。**

阶段 5 的最小目标：

```text
四路视频 + nvstreammux（batch=4）+ nvinfer（batch=4）
        ↓
nvtracker 目标追踪
        ↓
结构化 JSONL 输出（stream_id / track_id / class / bbox / confidence）
        ↓
每路 FPS 与基础 Metrics
```

阶段 5 当前不包含：

- RTSP；
- 事件系统；
- Kimi；
- DeepSeek；
- Agent；
- INT8。

### 后续阶段

- [ ] 阶段 6：事件系统、事件去重和关键帧抽取；
- [ ] 阶段 6：事件系统、事件去重和关键帧抽取；
- [ ] 阶段 7：Kimi + DeepSeek API、异步队列和降级策略；
- [ ] 阶段 8：Agent 白名单工具调用、验证、审计和回滚；
- [ ] RTSP 故障恢复；
- [ ] C++ 动态调度器；
- [ ] ftrace 和 CPU Affinity 分析；
- [ ] INT8 PTQ 与精度回归；
- [ ] 稳定性测试、Demo 和项目包装。

---

## 7. Windows、GitHub 与 Jetson 同步方式

项目采用“双通道同步”：

```text
代码、配置、脚本、Markdown 文档
Windows / Jetson → Git → GitHub → 另一端 git pull

ONNX、TensorRT Engine、视频、原始 Benchmark、Trace
Windows / Jetson → SCP / rsync → 目标设备
```

### 7.1 三端职责

| 位置 | 主要职责 | 可信产物 |
|---|---|---|
| Windows 主机 | PyTorch 权重下载、ONNX 导出、ONNX Checker、ORT 验证、文档整理 | `yolo11s.onnx`、`model_info.txt`、导出脚本 |
| GitHub | 代码、配置模板、脚本、README、CLAUDE、阶段报告的统一版本来源 | 可追踪的文本和源码 |
| Jetson | Engine 构建、DeepStream 集成、实机 Benchmark、运行和验收 | `.engine`、实机日志、性能结果 |

### 7.2 GitHub 中允许同步

- C++ / Python 源代码；
- CMake；
- YAML / JSON 配置模板；
- 导出、验证和 Benchmark 脚本；
- `models/model_info.txt`；
- README、CLAUDE 和阶段报告；
- Mock 测试数据；
- 小型汇总结果。

### 7.3 GitHub 中禁止同步

```text
*.pt
*.pth
*.onnx
*.engine
*.trt
*.mp4
*.h264
*.h265
.env
.env.*
API Key
完整 Base64 图片
大体积原始日志
完整 Trace
事件关键帧
```

### 7.4 标准同步流程

在任一设备开始工作前：

```bash
git status
git branch --show-current
git log -1 --oneline
```

只有工作区干净、分支正确，并且用户明确要求同步时，才执行：

```bash
git pull --ff-only
```

Windows 完成代码或文档更新后：

```powershell
git status
git add README.md CLAUDE.md scripts models\model_info.txt docs
git status
git commit -m "docs: sync stage 3 completion and stage 4 plan"
git push
```

Jetson 对齐 GitHub 后：

```bash
cd ~/JetEdge-Agent
git status
git pull --ff-only
cat CLAUDE.md
cat README.md
cat models/model_info.txt
sha256sum models/yolo11s.onnx
```

大文件通过 SCP：

```powershell
scp .\models\yolo11s.onnx .\models\model_info.txt seeed@192.168.3.200:~/JetEdge-Agent/models/
```

传输后必须比较 SHA256。GitHub 只同步“如何产生和验证模型”，不保存模型本体。

### 7.5 Claude Code 对齐顺序

Jetson 上的 Claude Code 每次开始新阶段前，按顺序读取：

```text
1. CLAUDE.md
2. README.md 的当前状态和同步规则
3. docs/ 中当前阶段相关计划或报告
4. models/model_info.txt
5. git status 和最近提交
6. Jetson 实机环境、插件、样例和日志
```

Claude Code 不得仅凭聊天上下文假设仓库已经同步，也不得把计划内容当作实测结果。

---

## 8. 推荐开发顺序

```text
环境核查与工程骨架
    ↓
单路本地视频硬件解码
    ↓
主机导出并验证 YOLO11s ONNX
    ↓
Jetson 构建 TensorRT FP16 Engine
    ↓
单路 nvinfer 检测验证
    ↓
四路视频 + nvstreammux + Tracker + Metrics
    ↓
事件系统和关键帧抽取
    ↓
Kimi / DeepSeek 异步智能分析
    ↓
RTSP 故障恢复与动态调度
    ↓
ftrace 和 CPU Affinity
    ↓
Control API、快照和回滚
    ↓
Agent 工具调用、验证和审计
    ↓
稳定性测试、Demo 和项目包装
```

不要从以下内容开始：

- 多 Agent；
- 大型 RAG；
- Jetson 本地常驻大参数模型；
- Linux 内核模块；
- 自定义 CUDA 全量后处理；
- 跨摄像头 ReID；
- 复杂多任务模型；
- 未建立 Benchmark 基线前的盲目优化。

---

## 9. YOLO11s 与大模型资源策略

### 9.1 YOLO11s 降负载

- 第一版固定输入 `1x3x384x640`；
- 第一版使用 FP16；
- 后续通过 `infer interval + Tracker` 降低检测频率；
- 关闭 OSD 进行纯性能 Benchmark；
- 根据 GPU、队列和温度状态动态调整推理间隔；
- INT8 仅在 FP16 稳定并有精度回归基线后开展。

建议配置档位：

```text
quality：YOLO11s，interval=0
balanced：YOLO11s，interval=1
performance：YOLO11n 或 YOLO11s，interval=2
```

具体档位只有实测通过后才能标记为可用。

### 9.2 大模型调用路由

```text
规则能够确定              → 本地处理，不调用大模型
视觉语义不确定            → Kimi
系统指标、日志或性能异常    → DeepSeek
高风险且需要系统操作        → 分析结果 + DeepSeek 候选计划 + 本地 Policy
```

### 9.3 Kimi 输入策略

- 默认只发送 1 张 ROI 关键帧；
- 必要时增加全景图或前后帧，最多 3 张；
- 本地缩放并 JPEG 压缩；
- 使用 pHash、目标位移或事件变化删除重复帧；
- 普通确认默认非思考模式；
- 高风险复杂事件才升级更强推理；
- 不发送无关连续视频。

### 9.4 DeepSeek 输入策略

- 不发送海量原始日志；
- 本地先聚合指标、Top-N 错误和相对基线变化；
- 仅在状态变化、明确异常或周期报告时调用；
- 固定 System Prompt 和 Schema 放在请求前缀；
- 普通诊断默认非思考模式；
- 限制输出 Token；
- Agent 必须等待完整 JSON 并通过 Schema 校验后才能执行。

### 9.5 延迟策略

- 本地事件立即告警，不等待云端响应；
- Kimi 和 DeepSeek 按事件类型路由，不默认串行；
- 互不依赖时并行调用；
- HTTP 客户端复用连接和 TLS 会话；
- Worker、连接池和模板在启动时初始化；
- 使用优先级队列处理高风险事件；
- API 超时、失败或熔断时，实时 Pipeline 继续运行。

---

## 10. 目标功能

### P0：必须完成

- C++ 多路视频接入；
- GStreamer / DeepStream Pipeline；
- Jetson 硬件解码；
- YOLO11s TensorRT FP16 推理；
- 多路 Batch；
- 目标追踪；
- 结构化 JSON 输出；
- RTSP 断流恢复；
- FPS、P50 / P95 / P99 延迟和丢帧统计；
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
- Kimi 关键帧事件复核；
- DeepSeek 性能诊断；
- Agent 性能优化；
- Agent 故障诊断；
- 自动 Benchmark；
- Docker 部署。

### P2：选做增强

- 短事件视频理解；
- CUDA 自定义后处理；
- 自定义 GStreamer 插件；
- 多任务模型；
- Linux 内核模块；
- 跨摄像头 ReID。

---

## 11. 项目目录

项目初期只创建当前阶段需要的目录，避免一次生成空壳。

```text
JetEdge-Agent/
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
│   ├── events/
│   ├── llm/
│   ├── scheduler/
│   ├── metrics/
│   ├── control/
│   └── common/
├── src/
│   ├── pipeline/
│   ├── inference/
│   ├── tracking/
│   ├── analytics/
│   ├── events/
│   ├── llm/
│   ├── scheduler/
│   ├── metrics/
│   └── control/
├── configs/
├── models/
│   └── model_info.txt
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

## 12. 环境核查

所有 Jetson 相关结论来自实机。只读核查不执行 `sudo apt update`：

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
gst-inspect-1.0 nvinfer 2>/dev/null | head -n 40

echo "===== Power Mode ====="
nvpmodel -q --verbose 2>/dev/null || true

echo "===== Hardware ====="
cat /proc/device-tree/model 2>/dev/null
```

核查结果写入 `docs/environment_report.md`。

---

## 13. 模型产物规则

### Windows 端

```text
models/yolo11s.onnx       不提交 Git
models/model_info.txt     提交 Git
scripts/export_*.py       提交 Git
scripts/verify_*.py       提交 Git
```

### Jetson 端

```text
models/yolo11s.onnx                         SCP 获取
models/yolo11s_*_fp16.engine                Jetson 本地生成，不提交 Git
models/model_info.txt                       Git 或 SCP 获取
```

TensorRT Engine 与 Jetson 的 TensorRT、CUDA、GPU 和构建参数绑定，不允许在 Windows 上构建后复制使用。

---

## 14. 测试视频

优先使用 Jetson 自带 DeepStream 样例视频：

```text
/opt/nvidia/deepstream/deepstream/samples/streams/
```

开发顺序：

```text
单路官方样例
    ↓
单路 YOLO11s nvinfer
    ↓
同一视频重复四路
    ↓
四段不同场景视频
    ↓
四路 RTSP
    ↓
抖动、丢包和断流测试
```

---

## 15. 构建方式

当前 C++ 工程构建方式：

```bash
cmake -S . -B build
cmake --build build -j2
```

运行命令以当前阶段真实实现为准。未实现的命令不得提前写成已可用。

---

## 16. Benchmark 设计

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
B9：事件系统 + 大模型异步分析
B10：Agent 查询、调优、验证和回滚
```

每组记录：

- Git 提交；
- Jetson、JetPack、CUDA、TensorRT 和 DeepStream 版本；
- 输入视频、编码、分辨率、FPS 和路数；
- 模型、精度、输入尺寸和 Batch；
- infer interval 和 Tracker；
- 输入、解码、推理和输出 FPS；
- P50 / P95 / P99 延迟；
- Batch 等待时间和填充率；
- 队列深度和丢帧率；
- CPU、GPU、RAM、温度和功耗；
- 大模型首 Token、总延迟、超时率和 Token 用量；
- Agent 控制、验证和回滚耗时。

---

## 17. 动态调度

调度器采用确定性的 C++ 状态机：

| 状态 | 运行策略 |
|---|---|
| Normal | 正常输入尺寸和推理频率 |
| Pressure | 增大推理间隔，使用 Tracker 补偿 |
| Thermal | 降低低优先级流推理频率 |
| Critical | 暂停低优先级流推理，禁止增加负载 |
| Recovery | 负载恢复后逐级恢复配置 |

调度器必须包含：进入阈值、退出阈值、滞回、最小保持时间、冷却时间和最大调整频率。

LLM 不负责实时调度状态机，只能在确定性控制能力建立后生成低频候选计划。

---

## 18. Agent 安全边界

Agent 只能调用白名单工具，例如：

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
- 权限和当前状态校验；
- 范围限制；
- 修改前保存快照；
- 执行超时；
- 审计日志；
- 修改后重新验证；
- 不达标自动回滚。

Agent 禁止：

- 执行任意 Shell；
- 调用 `sudo`；
- 任意读写系统文件；
- 绕过 C++ 状态机；
- 关闭全部关键流；
- 在过热或 Critical 状态增加负载；
- 未经验证宣布优化成功；
- 读取、输出或上传完整 API Key。

---

## 19. 预期 Demo

### Demo 1：四路视频基础能力

四路检测、追踪 ID、每路 FPS、系统指标、单流断开和自动恢复。

### Demo 2：事件理解

本地规则触发事件，抽取关键帧，Kimi 输出场景描述、风险和误报判断；本地告警不等待 Kimi。

### Demo 3：性能诊断

人为制造 FPS 下降，DeepSeek 分析聚合指标和错误摘要，生成调整推理间隔的候选计划。

### Demo 4：安全 Agent

Policy 校验候选工具，保存快照，执行低风险调整，重新 Benchmark，验证结果并保留或回滚。

---

## 20. 验收标准

### 阶段 4（2026-08-01 验收通过）

- [x] 确认 Jetson TensorRT / DeepStream / GStreamer 实机版本（TRT 10.3.0 / DS 7.1.0 / GStreamer 1.20.3）；
- [x] `trtexec` 能解析 `yolo11s.onnx`；
- [x] FP16 Engine 构建成功（21.81 MiB，无 warning，SHA256 `c6cc41d0...a82274a`）；
- [x] Engine 输入输出 Binding 符合预期（images `1x3x384x640` → output0 `1x84x5040`）；
- [x] 单路本地视频硬件解码成功；
- [x] `nvinfer` 加载 Engine 成功；
- [x] YOLO11 输出解析正确（自写自定义 parser，输出实测为绝对像素坐标 + 已 sigmoid 的 class scores）；
- [x] 检测框、类别和置信度合理（每帧 8-16 个目标，bus conf=0.95 / car conf=0.94，与 ground truth 吻合）；
- [x] Pipeline 正常处理 EOS；
- [x] Ctrl-C 安全退出；
- [x] 无明显持续内存增长（RSS 306.6 → 307.4 MiB）。

### 最终基础能力

- 4 路视频稳定接入；
- 检测、追踪和事件统计正常；
- 单流故障不影响其他流；
- RTSP 恢复后自动重连；
- 连续运行至少 2 小时；
- 无明显内存持续增长。

### Agent 能力

- 查询真实指标；
- 调用真实白名单工具；
- 完成配置修改；
- 修改后重新验证；
- 失败自动回滚；
- 操作有审计日志；
- Agent 或大模型故障不影响底层 Pipeline；
- 报告只使用实测数据。

---

## 21. 开发约束

仓库级开发规则见 `CLAUDE.md`。关键约束：

- 不执行或建议执行 `sudo apt update`；
- 未经明确许可不安装系统软件；
- DeepStream、TensorRT 和 GStreamer API 必须通过 Jetson 实机确认；
- 一次只实现一个阶段；
- 不覆盖未提交代码；
- 不打印或提交 API Key；
- 不把模型、Engine 和视频提交 Git；
- 每次修改必须给出构建、运行和验收方式；
- 未在 Jetson 上实际验证的功能不得标记为完成。

---

## 22. 文档索引

| 文档 | 说明 |
|---|---|
| `CLAUDE.md` | Claude Code 仓库级开发、安全和同步规则 |
| `README.md` | 项目定位、当前进度、同步方式和总体路线 |
| `models/model_info.txt` | YOLO11s ONNX 输入输出、版本、验证和 SHA256 |
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

## 23. 项目成果目标

最终计划输出：

- C++ DeepStream 多路推理程序；
- YOLO11s TensorRT FP16 / INT8 Engine；
- 多路 RTSP 测试环境；
- Tracker 和事件统计；
- RTSP 故障恢复模块；
- Metrics Exporter；
- Grafana Dashboard；
- 动态调度器；
- Kimi 多模态事件复核；
- DeepSeek 性能和故障诊断；
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

## 24. License

项目许可证将在代码公开前确定。
