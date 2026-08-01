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

---

## 阶段 4：TensorRT FP16 Engine + 单路 nvinfer 验证（已完成）

**日期**：2026-08-01

### 环境核查（已完成，全部实测）

| 检查项 | 实测值 |
|---|---|
| OS / L4T | Ubuntu 22.04.5 LTS / R36.4.3 |
| JetPack | 6.2.1+b38 |
| CUDA | 12.6.68（`/usr/local/cuda/bin/nvcc`） |
| TensorRT | 10.3.0（`/usr/src/tensorrt/bin/trtexec`，`--version` 不带模型参数时打印帮助并报 "Model missing"，属正常） |
| DeepStream | 7.1.0（cuDNN 9.0） |
| GStreamer | 1.20.3 |
| 功耗模式 | MAXN_SUPER（mode 2），6 核在线，GPU 固定 1020 MHz |
| nvinfer 插件 | 存在，版本 7.1.0；关键属性：`config-file-path`、`batch-size`(1-1024)、`gpu-id`、`process-mode`(primary 默认)、`unique-id`(默认 15)。`infer-interval` 不是插件属性，是配置文件键 |
| 样例配置 | `/opt/nvidia/deepstream/deepstream-7.1/samples/configs/deepstream-app/config_infer_primary.txt`（ONNX 接入字段：`onnx-file` / `model-engine-file` / `network-mode`(0=FP32,1=INT8,2=FP16) / `num-detected-classes` / `cluster-mode`(2=NMS) / `parse-bbox-func-name` / `custom-lib-path`） |
| 样例源码 | `deepstream-test1`~`test5`、`deepstream-app`（`sources/apps/sample_apps/`） |
| 样例视频 | `sample_1080p_h264.mp4`（1080p30，48.1s）、`sample_720p.h264`（裸流，720p30，40.1s） |

**关键发现**：

1. 本机无 YOLO 样例解析器：`sources/libs/nvdsinfer_customparser/nvdsinfer_custombboxparser.cpp` 只有 8 种解析函数（BatchedNMSTLT / DDETRTAO / EfficientDetTAO / MrcnnTLT / MrcnnTLTV2 / NMSTLT / Resnet / TfSSD），**无任何 YOLO 系列函数**。Stage 4 必须自写 YOLO11 自定义 parser（编译 .so 后通过 `custom-lib-path` + `parse-bbox-func-name` 接入），且必须基于实际输出张量 `1x84x5040` 验证，不能套用旧解析器。
2. 模型位置：`/home/seeed/JetEdge-Agent/models/yolo11s.onnx`（37,944,117 字节 ≈ 36.19 MB），SHA256 = `41abd2ff906712b41c60de9b7d5d5f09918e23a331d80cc0926071600fd3e078` ✓ 与 model_info.txt 一致。
3. 模型路径（`/home/seeed/JetEdge-Agent/models/`）与 git 仓库（`/home/seeed/projects/jetedge-agent/`）是**两个不同目录**；仓库 `models/` 下只有 `model_info.txt`（无 onnx，符合 Git 规则）。
4. `model_info.txt`：仓库版与 Jetson 端内容一致（均含 `NaN/Inf check: PASSED`，该行无换行符结尾），仅行尾符不同（仓库 LF / Jetson CRLF，Windows 生成所致），无内容差异。

### FP16 Engine 构建（已完成，实测）

构建命令（TRT 10.3，`--workspace` 已弃用，使用 `--memPoolSize`）：

```bash
/usr/src/tensorrt/bin/trtexec \
  --onnx=/home/seeed/JetEdge-Agent/models/yolo11s.onnx \
  --saveEngine=/home/seeed/JetEdge-Agent/models/yolo11s_b1_384x640_fp16.engine \
  --fp16 \
  --memPoolSize=workspace:2048
```

| 项目 | 实测值 |
|---|---|
| ONNX SHA256（构建前） | `41abd2ff...fd3e078` ✓ 与 model_info.txt 一致 |
| ONNX 解析 | ONNX IR 0.0.7 / opset 12 / producer pytorch 2.13.0，解析耗时 0.103 s |
| 网络张量 | Detected 1 inputs and 3 output network tensors；engine binding 为 images `1x3x384x640` + output0 `1x84x5040` ✓ 与 model_info.txt 一致 |
| 构建耗时 | 365.1 s（约 6 分钟） |
| Engine 大小 | 22,866,804 字节 ≈ 21.81 MiB（trtexec 报告 Created engine size 21.8075 MiB） |
| Engine SHA256 | `c6cc41d01d906427bf39bddace5fc0b84f2935ae17a0c0e1da504e0ffa82274a` |
| Warning | 无 |
| 推理性能（随机输入，batch 1） | Throughput 216.7 qps；Host latency mean 4.94 ms / P95 4.96 ms / P99 4.97 ms；GPU compute mean 4.61 ms |
| 设备 | Orin（CC 8.7，8 SM，7.6 GiB 显存，MAXN_SUPER 1.02 GHz） |
| 日志 | `/home/seeed/JetEdge-Agent/models/trtexec_build_20260801.log`（18,371 字节，不入 Git） |

### YOLO11 自定义 parser 与 nvinfer 集成（已完成，实测）

**输出格式实测结论**（ONNX Runtime 对真实图片推理 + ONNX 图分析双重验证）：

1. 输出 `1x84x5040`：通道 0-3 = bbox (cx, cy, w, h) **绝对像素坐标**（模型输入空间 640x384 内），通道 4-83 = class scores **已在模型内 sigmoid**（[0,1]）。整个 decode（DFL Softmax → 线性组合 → stride 乘法）已内置在 ONNX 图中（`/model.23/Mul_2` + `/model.23/Sigmoid`），parser 无需任何 sigmoid/stride 数学。
2. 同一 bus 目标在不同 row 的 cell 中 cy 值相同（189.6 vs 189.9）→ 证明是绝对像素坐标而非 grid 相对偏移。

**实现文件**：

| 文件 | 说明 |
|---|---|
| `src/inference/yolo11_parser.cpp` | 自定义 parser .so（`NvDsInferParseCustomYolo11`），支持 FP32/FP16 输出 buffer，读 dims 时用 `dims.d[0]`=channels + `numElements`（DeepStream 传 2 维 dims `[84,5040]`，不含 H/W） |
| `src/inference/metadata_probe.cpp` | nvinfer src pad 探针，读取 NvDsBatchMeta，打印每帧检测数和最高置信度目标 |
| `configs/nvinfer_yolo11s_fp16.txt` | nvinfer 配置（`net-scale-factor=1/255`、`cluster-mode=2` NMS、`custom-lib-path`/`parse-bbox-func-name`） |
| `configs/coco_labels.txt` | COCO 80 类 |
| `configs/streams_stage4.yaml` | 单路推理验证配置 |

**调试中发现并修复的问题**：

1. **parser 返回 false 导致 nvinfer SIGSEGV**：`getDimsCHWFromDims` 假定 CHW 布局，但 DeepStream 传的 dims 是 `[84, 5040]`（无 H/W）→ 改用 `dims.d[0]` + `numElements/channels` 计算。
2. **检测错乱（175+ 目标/帧，检出 oven/sink 等）**：nvinfer 输入 0-255 原始像素，但模型期望 0-1 归一化 → 加 `net-scale-factor=0.00392156862745098`（1/255）后恢复正常（8-16 目标/帧，bus/car 高置信）。Python 对照实验证实：0-255 输入时 conf>0.1 目标从 78 暴涨到 1317。
3. **`custom-lib-path` / `labelfile-path` 相对路径**按 config 文件目录解析 → 改为绝对路径。

### 验收结果（全部实机实测）

| 检查项 | 结果 |
|---|---|
| parser .so 编译 | 0 warning |
| nvinfer 加载 engine | `deserialized trt engine ... sucessfully` |
| 单路 720p 视频 | 1440 帧全部处理，~12s（sync=false） |
| 检测数量 | 每帧 8-16 个目标（合理） |
| 检测内容 | class=5 bus conf=0.95（与 Python ground truth 的 box=[235,1 404x382] 吻合）、class=2 car conf=0.94 |
| EOS | `EOS received, quitting main loop` → exit OK |
| Ctrl-C | `received SIGINT ... exit OK` |
| 内存 | RSS 306.6 → 307.4 MiB（稳定） |
| 4 路 baseline 回归 | 正常（inference=false 路径不受影响） |

### 遗留说明

- Stage 4 只验证单路（batch=1）。`configs/streams.yaml` 的 inference 保持 `enable: false`（4 路 mux 与 nvinfer batch-size=1 不匹配属预期）。
- 坐标空间为模型输入 640x384；letterbox 还原到原始分辨率由后续阶段处理（当前 mux 输出 640x384 直接匹配模型输入，无 letterbox 变换）。
- 检测框合理性已通过数值对比验证；可视化（OSD）留待需要时再做。
