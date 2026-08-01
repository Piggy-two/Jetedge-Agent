# JetEdge-Agent 开发日志

按阶段记录：每个阶段记录实际结果、验收状态和遗留问题。不要将计划中的数字写成实测结果。

> **阶段编号说明（2026-08-01）**：本日志早期采用旧方案编号（阶段 0→1→2→3）。自 2026-08-01 起，阶段编号对齐 `README.md` 新方案。旧方案"阶段 2"（四路 streammux）已完成，在新方案中对应 Stage 5 的前置工作。当前待实施的是新方案 **Stage 4**：TensorRT FP16 Engine + 单路 DeepStream `nvinfer` 验证（不含 Tracker、不含四路检测）。

---

## 阶段 0：只读环境核查与项目初始化

**日期**：2026-07-31

### 完成内容

- [x] 只读核查环境（OS/L4T/JetPack/CUDA/TensorRT/DeepStream/GStreamer/插件/功耗模式/内存/磁盘/工具链）
- [x] 定位 DeepStream 样例源码和样例视频
- [x] `docs/environment_snapshot.md` 环境快照
- [x] `scripts/check_environment.sh` 只读核查脚本
- [x] 最小 C++17 + CMake 工程骨架（`apps/jetedge_server/main.cpp` 仅打印版本信息）
- [x] git 仓库初始化

### 实测结果

| 检查项 | 实测值 |
|---|---|
| 设备 | Jetson Orin Nano Super 开发套件（8GB） |
| OS / L4T | Ubuntu 22.04.5 LTS / R36.4.3 |
| JetPack | 6.2.1+b38 |
| CUDA / TensorRT | 12.6.68 / 10.3.0 |
| DeepStream / GStreamer | 7.1.0 / 1.20.3 |
| 功耗模式 | MAXN_SUPER（GPU 固定 1.02 GHz） |
| `cmake -S . -B build` | 成功 |
| `cmake --build build -j2` | 成功 |
| `./build/jetedge_server` | 输出版本信息，exit=0 |
| 关键插件 | nvv4l2decoder / nvstreammux / nvinfer / nvtracker / nvdsanalytics / nvurisrcbin 均存在 |

### 验收状态

阶段 0 验收标准全部通过。

### 遗留问题 / 下一步

- ~~阶段 1 输入视频~~ → 已完成
- ~~阶段 1 需要确认 demux 动态 pad 处理方式~~ → 已完成

---

## 阶段 1：单路本地视频最小 Pipeline

**日期**：2026-07-31

### 完成内容

- [x] PipelineConfig 结构体（输入路径）
- [x] Pipeline 类：build / run / quit
- [x] 支持裸 `.h264` 流：`filesrc → h264parse → nvv4l2decoder → fakesink`
- [x] 支持 `.mp4` 容器：`filesrc → qtdemux →(动态 pad)→ h264parse → nvv4l2decoder → fakesink`
- [x] qtdemux `pad-added` 信号按 caps 过滤，只取视频流，跳过音频流
- [x] GstBus 处理：ERROR（含 debug 信息）、EOS、WARNING、STATE_CHANGED
- [x] GLib `g_unix_signal_add` 安全处理 SIGINT/SIGTERM，优雅退出
- [x] 结构化日志：`timestamp | level | module | stream_id | state | operation | error_code | message`
- [x] 错误路径：无效路径输出明确错误 + 错误码 "STATE001"

### 实测结果

| 检查项 | 结果 |
|---|---|
| `sample_720p.h264`（裸流）| 硬件解码成功，~3s 跑完 40s 视频，EOS 正常退出 |
| `sample_1080p_h264.mp4`（容器）| qtdemux 动态 pad 正确过滤音频流，视频流正常解码 ~5s |
| 无效路径错误处理 | `failed to set pipeline to PAUSED` + exit=1 |
| SIGINT 优雅退出 | Pipeline → NULL，资源释放，exit OK |
| 连续 10 次运行 | 全部成功，无崩溃 |
| 编译 | `cmake --build build -j2` 成功（0 warning） |
| NVMM 内存 | 确认使用硬件解码（`memory:NVMM` caps） |

### 新增/修改文件

| 文件 | 操作 |
|---|---|
| `include/jetedge/common/logging.h` | 新建 — 结构化日志宏 |
| `include/jetedge/pipeline/pipeline_config.h` | 新建 — PipelineConfig |
| `include/jetedge/pipeline/pipeline.h` | 新建 — Pipeline 类 |
| `src/pipeline/pipeline.cpp` | 新建 — Pipeline 实现 |
| `apps/jetedge_server/main.cpp` | 修改 — 命令行 + 信号处理 + Pipeline |
| `CMakeLists.txt` | 修改 — 新源文件 + GLib 依赖 |

### 验收状态

阶段 1 验收标准全部通过。

### 下一步

阶段 2：四路本地视频与 nvstreammux（已完成）。

---

## 阶段 2：四路本地视频与 StreamMux

**日期**：2026-07-31

### 完成内容

- [x] `SourceBin` 类：封装单路 `filesrc → [qtdemux] → h264parse → nvv4l2decoder` 链路
- [x] `SourceManager` 类：管理 N 个 SourceBin，请求/链接 nvstreammux request pad
- [x] `ConfigLoader`：yaml-cpp 解析 `configs/streams.yaml`
- [x] `StreamConfig` / `MuxConfig` / `StreamPriority` 数据结构
- [x] 每路独立 decoder src pad probe 帧计数
- [x] 动态 pad 处理（qtdemux pad-added，按 video caps 过滤）
- [x] 无推理、无 Tracker、无 RTSP

### 实测结果

| 检查项 | 结果 |
|---|---|
| 1 路 YAML 配置 | 1442 帧，EOS 正常退出 |
| 2 路 YAML 配置 | 2×1442=2884 帧，两个 source_id EOS 正确处理 |
| 4 路 YAML 配置 | 4×1442=5768 帧，四个 source_id EOS 正确处理，无死锁 |
| 无效路径 | 错误明确输出 "failed to set pipeline to PAUSED"，exit=1 |
| 连续 10 次 4 路运行 | 全部成功，无崩溃、无内存持续增长 |
| 编译 | `cmake --build build -j2` 成功（0 warning） |

### 每路帧计数

4 路同时运行时，每路均正确解码全部 1442 帧（720p, 30fps, ~48 秒视频），总吞吐 ~5768 帧/~12s ≈ **~480 fps**（纯解码+mux，无推理）。

### 新增/修改文件

| 文件 | 操作 |
|---|---|
| `include/jetedge/pipeline/stream_config.h` | 新建 — StreamConfig / MuxConfig / StreamPriority |
| `include/jetedge/pipeline/source_bin.h` | 新建 — SourceBin 类 |
| `include/jetedge/pipeline/source_manager.h` | 新建 — SourceManager 类 |
| `include/jetedge/common/config_loader.h` | 新建 — YAML 配置加载器 |
| `src/pipeline/source_bin.cpp` | 新建 |
| `src/pipeline/source_manager.cpp` | 新建 |
| `src/common/config_loader.cpp` | 新建 |
| `configs/streams.yaml` | 新建 — 4 路示例配置 |
| `include/jetedge/pipeline/pipeline.h` | 重写 — 使用 SourceManager |
| `src/pipeline/pipeline.cpp` | 重写 |
| `apps/jetedge_server/main.cpp` | 重写 — YAML 配置驱动 |
| `CMakeLists.txt` | 更新 — 新文件 + yaml-cpp 依赖 |
| `include/jetedge/common/logging.h` | 修复 — 添加 `<cstdarg>` |

### 验收状态

阶段 2 验收标准全部通过。

### 下一步

**新方案 Stage 4**：TensorRT FP16 Engine + 单路 DeepStream `nvinfer` 验证。

注意：新方案 Stage 4 只包含：
1. 在 Jetson 上检查 TensorRT/DeepStream/nvinfer 实际环境
2. 基于已验收 ONNX 构建 FP16 Engine
3. 记录构建命令、版本、binding、warning、Engine 大小和 SHA256
4. 单路本地视频跑通 nvinfer
5. 验证 YOLO11 输出解析、检测框、类别、置信度、EOS、Ctrl-C 和内存行为

暂不包含：四路视频、Tracker、Metrics、RTSP、事件系统、Kimi、DeepSeek、Agent、INT8。

旧方案"阶段 2"代码（四路 streammux + fakesink）已实现，将在新方案 Stage 5 中复用。
