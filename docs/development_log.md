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

---

## 阶段 5：四路检测 + Tracker + 结构化 JSONL + per-stream Metrics（已完成）

**日期**：2026-08-01

### 阶段目标（对齐 README 新方案）

```text
四路视频 + nvstreammux（batch=4）+ nvinfer（batch=4）
        ↓
nvtracker 目标追踪
        ↓
结构化 JSONL 输出（stream_id / track_id / class / confidence / bbox）
        ↓
每路 FPS 与基础 Metrics
```

不包含：RTSP、事件系统、Kimi、DeepSeek、Agent、INT8、自适应调度。

### 1. 派生 batch-dynamic ONNX（Jetson 实机，实测）

已验收 ONNX（`yolo11s.onnx`，SHA256 `41abd2ff...fd3e078`）输入为静态 `1x3x384x640`，无法直接构建 batch=4 engine。派生工作流（`scripts/make_batch_dynamic_onnx.py`）：

1. 输入/输出 batch 维符号化（"batch"）；
2. **扫描发现并修复 6 个 hard-coded batch=1 的 Reshape target initializer**（首元素 1 → 0，ONNX Reshape 语义 0=复制输入对应维）：
   - `/model.10/m/m.0/attn/Constant_output_0`（C2PSA attn [1,4,128,240]）
   - `/model.10/m/m.0/attn/Constant_3_output_0`（C2PSA attn [1,256,12,20]）
   - `/model.23/Constant_output_0`（stride 展平 [1,64,-1]）
   - `/model.23/Constant_3_output_0`（[1,64,-1]）
   - `/model.23/dfl/Constant_output_0`（DFL head [1,4,16,5040]）
   - `/model.23/dfl/Constant_1_output_0`（[1,4,16,5040]）

   调试发现过程：先只修 3 个 → ORT batch=4 推理报 DFL Reshape `{1,64,20160} → [1,4,16,5040]` 失败；改为"扫描所有被 Reshape 引用的 int64 常量且首元素为 1"后全部修复。
3. ORT 1.23.2（CPU）验证：
   - 派生 ONNX batch=1 输出与原 ONNX **完全一致**（max diff = 0.0）—— 权重未变，语义保持；
   - batch=4 推理输出 `4x84x5040`，PASSED；
   - batch=1 vs batch=4 同 slice 差异 ~3e-4（CPU 算子 kernel 差异，不影响 sigmoid 后分类阈值判定）。

产物：`yolo11s_dynamic.onnx`（37,944,131 B，SHA256 `fa27873a74571f0f5546a0d4d9b1658e8bb34367f7b58777ae09b0b03b766e48`）。

### 2. 构建 batch=4 FP16 Engine（Jetson 实机，实测）

```bash
/usr/src/tensorrt/bin/trtexec \
  --onnx=/home/seeed/JetEdge-Agent/models/yolo11s_dynamic.onnx \
  --saveEngine=/home/seeed/JetEdge-Agent/models/yolo11s_b4_384x640_fp16.engine \
  --fp16 --memPoolSize=workspace:2048 \
  --minShapes=images:1x3x384x640 --optShapes=images:4x3x384x640 --maxShapes=images:4x3x384x640
```

| 项目 | 实测值 |
|---|---|
| Engine 大小 | 22,278,268 B ≈ 21.25 MiB |
| Engine SHA256 | `136bd5fdf7eed35716e35337a3d941dd735c2e9c856c84b8be46cea78b06818d` |
| 构建耗时 | 494.2 s（约 8.2 分钟） |
| Binding | images `4x3x384x640`（opt）→ output0 `4x84x5040`（opt），MIN=1 / MAX=4 |
| Warning | 1 条 DLA-fallback warning（无影响） |
| 日志 | `/home/seeed/JetEdge-Agent/models/trtexec_build_b4_20260801.log`（不入 Git） |

### 3. 代码变更

| 文件 | 操作 | 说明 |
|---|---|---|
| `include/jetedge/pipeline/stream_config.h` | 修改 | 新增 `TrackerConfig`（enable/ll_lib_file/ll_config_file/width/height/gpu_id）、`OutputConfig`（jsonl_path/labels_file_path/fps_report_interval_sec） |
| `include/jetedge/common/config_loader.h` + `src/common/config_loader.cpp` | 修改 | 解析 `tracker:` / `output:` YAML 段 |
| `include/jetedge/metrics/metrics_registry.h` + `src/metrics/metrics_registry.cpp` | 新建 | per-stream 计数器 + 2s 滑动窗口 FPS（input/inference/output 三阶段）+ 全程平均，互斥锁保护 |
| `include/jetedge/inference/metadata_probe.h` + `src/inference/metadata_probe.cpp` | 重写 | 三个 probe：input（nvinfer sink）/ infer（nvinfer src）/ output（nvtracker src）；output probe 输出 JSONL（ts_ms/stream_id/frame_num/track_id/class_id/class/confidence/bbox）+ 更新 metrics；`load_label_file` 支持 `"id name"` 与纯名字两种格式 |
| `include/jetedge/pipeline/pipeline.h` + `src/pipeline/pipeline.cpp` | 重写 | 链接 streammux → nvinfer → nvtracker → fakesink；三 probe 安装与清理；周期 FPS 报告（g_timeout_add_seconds）；最终统计表格 |
| `apps/jetedge_server/main.cpp` | 修改 | 传递 tracker/output 配置 |
| `CMakeLists.txt` | 修改 | 加入 metrics 模块 |
| `scripts/make_batch_dynamic_onnx.py` | 新建 | batch-dynamic ONNX 派生脚本（可复现，记录 SHA256） |
| `scripts/analyze_stage5_jsonl.py` | 新建 | JSONL 分析：每流帧数/检测数/track 稳定性/类别分布 |
| `configs/nvinfer_yolo11s_b4_fp16.txt` | 新建 | batch-size=4 nvinfer 配置 |
| `configs/streams_stage5.yaml` | 新建 | 4 路不同视频 + tracker + output 配置 |
| `models/model_info.txt` | 修改 | 记录派生 ONNX 与 b4 Engine 信息 |

调试中修复的问题：
1. **labels 解析 bug**：`coco_labels.txt` 是纯名字列表（行号=class_id），初版解析器按 `"id name"` 解析 → "hair drier" 被解析成 id=0/name="drier"、其余全部丢弃（JSONL 中 class 全为 "?"）。改为两种格式自动识别后，class 分布正确（cam1 car/bus/truck 与 ground truth 吻合）。
2. **avg FPS 计算 bug**：窗口滚动后全程平均用了"当前窗口开始时间"作分母，导致 cam1 显示 1595 fps。增加 `run_start_ns`（注册时记录），avg = 总帧数 / (now - run_start)。
3. **input probe 位置**：初版装在 nvinfer src（实为推理后），按 Stage 5 三阶段要求改为 nvinfer sink（input）/ nvinfer src（infer）/ nvtracker src（output）。

### 4. 实机验收结果（全部实测）

| 检查项 | 结果 |
|---|---|
| nvinfer 加载 b4 engine | `deserialized trt engine` 成功；dynamic profile min 1x3x384x640 / opt 4x3x384x640 / max 4x3x384x640 |
| nvtracker | `libnvds_nvmultiobjecttracker.so` 初始化成功（NvDCF_perf 配置） |
| 4 路不同视频 | cam1 1442 帧 / cam2 163 / cam3 288 / cam4 179，共 2072 帧，EXIT=0 |
| 4 路同一视频 | 4×1442=5768 帧，每路 17248 检测、obj/frame=11.96 完全一致 |
| 每流 FPS | 4 路同视频：in=infer=out=52.84 fps/流（~211 fps 总吞吐）；4 路不同视频：cam1 ~44 fps |
| track_id 稳定 | cam1 track=60 连续 701 帧（gaps=0）；cam2 track=0/1 各 161 帧；cam4 track=2/3 各 177 帧 |
| class 分布 | cam1 car 10119/bus 179/truck 550；cam2 person 6549；cam3 person；cam4 bicycle/skateboard/backpack —— 与各场景匹配 |
| 置信度 | bus conf=0.955（Stage 4: 0.95）；car conf 0.58-0.97 |
| bbox 坐标还原 | bus [235,1,404,382]@640x384 → [469.05,1.96,805.56,716.23]@1280x720（×2.0 / ×1.875，误差 <1px，nvinfer 拉伸缩放映射精确） |
| JSONL | 4 路不同视频 run 输出 18,333 行 → `logs/stage5_detections.jsonl` |
| 周期报告 | 每 5 s 打印每流 in/infer/out FPS + 检测数（短流 EOS 后保持统计） |
| EOS | 4 路 EOS 分别处理（`Successfully handled EOS for source_id=0..3`）+ 总 EOS 优雅退出；短流先 EOS 后 batch 不满，dynamic engine 按实际 batch 推理无报错 |
| Ctrl-C | SIGINT → 优雅退出，EXIT=0 |
| 内存 | 3 次连续运行：起始 RSS 610-611 MB，15 s 收敛 ~615 MB；跨运行无残留增长（无泄漏）；相比 Stage 4 单路 306 MB 增加为 4 路解码 + 推理 + tracker 的正常开销 |

### 5. 遗留说明

- 4 路不同视频总时长由 cam1 决定（本配置 ~33 s）；连续 2 小时稳定性测试属于最终验收项，未执行。
- 坐标空间为 mux 输出 1280x720；nvinfer 对 720p 输入非等比拉伸到 640x384 推理，bbox 精确映射回 720p（数值验证）。若后续需要等比 letterbox，需在 nvinfer 或 source 侧配置。
- `streams.yaml`（Stage 2 baseline）的 inference 仍保持 `enable: false`；Stage 5 使用 `streams_stage5.yaml`。
- JSONL 的 confidence 为检测置信度（nvtracker 保留 obj_meta->confidence）；tracker 自身置信度（shadow 状态等）未输出，留待 Stage 6 事件系统需要时补充。
- 下一阶段 Stage 6（事件系统、去重、关键帧）尚未开始。
