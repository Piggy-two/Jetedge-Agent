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

---

## 2026-08-01 Stage 6:事件系统、事件去重与关键帧抽取(验收通过)

### 1. 目标

四路检测链路上加入规则事件(appearance/disappearance/count/zone)、事件去重状态机、事件 JSONL 与事件触发的整帧关键帧 JPEG 保存;每流事件统计并入周期 metrics。

### 2. 实现文件

| 文件 | 操作 | 说明 |
|---|---|---|
| `include/jetedge/events/event_types.h` | 新建 | EventRecord/ObservedObject/EventType |
| `include/jetedge/events/event_engine.h` + `src/events/event_engine.cpp` | 新建 | 纯逻辑状态机(去重/grace/滞回/zone/计数) |
| `include/jetedge/events/event_writer.h` + `src/events/event_writer.cpp` | 新建 | 事件 JSONL 写入 |
| `include/jetedge/events/keyframe_writer.h` + `src/events/keyframe_writer.cpp` | 新建 | 整帧 JPEG 保存(官方 nvds_obj_enc) |
| `include/jetedge/events/event_probe.h` + `src/events/event_probe.cpp` | 新建 | tracker src pad 探针:适配→引擎→写入器 |
| `src/pipeline/pipeline.cpp` | 修改 | events 集成、探针安装/移除、EOS/Ctrl-C flush |
| `src/common/config_loader.cpp` + `include/jetedge/common/config_loader.h` | 修改 | `events:` 配置段解析 |
| `configs/streams_stage6.yaml` | 新建 | Stage 5 四路 + events 段 |
| `tests/test_event_engine.cpp` | 新建 | 单元测试(ALL PASS) |
| `CMakeLists.txt` | 修改 | 事件模块、链接 nvds_batch_jpegenc |

### 3. 关键帧取帧攻关(重要)

NVMM batch buffer 在 Jetson 上不能按普通像素 buffer 处理,三次迭代:

1. `gst_buffer_map` 直读像素 → `map.data` 是 `NvBufSurface*`(64 B 结构体),非像素;
2. `NvBufSurfaceMap`+`NvBufSurfaceSyncForCpu`(PITCH 可 CPU 映射,与树内 gst-dsexample 一致)+ `NvBufSurface2Raw` 兜底(BLOCK_LINEAR)→ 实测 batch 内 layout 混合,`NvBufSurface2Raw` 的 UV plane 复制失败;
3. 官方 `nvds_obj_enc_process(isFrame=1)`(`libnvds_batch_jpegenc`,test4/image-meta-test 实机模式)→ GPU 编码任意 layout 整帧为 JPEG,`NVDS_CROP_IMAGE_META` 读回写文件。surface 定位用 `frame_meta->batch_id`(nvdsmeta.h 文档化)。

### 4. 实机验收结果(全部实测)

| 检查项 | 结果 |
|---|---|
| 4 路不同视频 | cam1 1442 / cam2 163 / cam3 288 / cam4 179,共 2072 帧,EXIT=0 |
| 事件 JSONL | 1194 行,逐行 JSON 校验 0 失败 |
| 事件分布 | cam1 appearance 369/disappearance 369/count_high 33/count_exit 32/zone_entry 369 |
| 关键帧 | 150 次保存(cap)、0 错误;内容 SSIM:cam1 vs 源帧 0.985,cam2 vs office 0.976 / vs bus/car -0.028 |
| EOS / Ctrl-C | 每流 flush + 优雅退出 EXIT=0 |
| 内存 | RSS 619.8 → 628.0 MB 收敛 |
| 单元测试 | ALL PASS |

### 5. 修复的 bug

1. 事件 JSONL `keyframe` 字段裸值无引号 → 整行非法 JSON;
2. 事件 JSONL `zone` 字段裸值无引号 → zone_entry 行非法 JSON;
3. NVMM buffer 像素误读(见攻关迭代 1);
4. BLOCK_LINEAR surface 的 NvBufSurface2Raw UV plane 失败(见攻关迭代 2)。

### 6. 遗留

- zone 配置过大导致 zone_entry≈appearance(配置可调);
- 关键帧同步编码+写盘在探针线程,事件风暴时可能阻塞实时线程(异步化属后续阶段);
- 2 小时稳定性测试属于最终验收项。

详细报告:`docs/stage6_events.md`。

---

## 2026-08-01 Stage 7: API 密钥管理与 Provider 策略变更

### 策略变更

Stage 7 原定使用 **Kimi + DeepSeek** 双 Provider，现调整为 **Qwen (通义千问) + DeepSeek**:

| 项目 | 原方案 | 现方案 | 原因 |
|---|---|---|---|
| 视觉事件复核 | Kimi | Qwen3-VL 8B Instruct | 性价比更高($0.08/M 输入 token)，Apache 2.0 开源可自部署 |
| 文本诊断 | DeepSeek | DeepSeek | 不变 |
| API 端点 | — | Qwen: DashScope (`dashscope.aliyuncs.com`); DeepSeek: `platform.deepseek.com` | — |

### API 密钥管理

- 密钥文件: `~/.jetedge/secrets.env` (权限 600, 不进 Git)
- 环境变量: `QWEN_API_KEY` / `DEEPSEEK_API_KEY`
- 代码接口: `include/jetedge/common/secrets.h` → `qwen_api_key()` / `deepseek_api_key()`
- `src/common/secrets.cpp`: 优先读环境变量, 回退解析 secrets 文件; 永不打印 key 值

### 新增文件

| 文件 | 操作 | 说明 |
|---|---|---|
| `~/.jetedge/secrets.env` | 新建 | API key 存储(不进 Git) |
| `include/jetedge/common/secrets.h` | 新建 | Secrets 加载接口 |
| `src/common/secrets.cpp` | 新建 | 从环境变量或文件读取 key |
| `CMakeLists.txt` | 修改 | 加入 secrets.cpp |

### 下一步

- `llm_types.h` — LlmProvider enum + LlmConfig 结构体
- `llm_queue.h/cpp` — 有界优先级异步请求队列
- `llm_client.h/cpp` — libcurl HTTP 客户端(Qwen + DeepSeek, 超时/重试/熔断)
- `llm_router.h/cpp` — 事件类型 → Provider 路由

---

## 2026-08-01 Stage 7：Qwen + DeepSeek 异步分析（验收通过）

### 1. 目标

事件路由（本地规则本地化；zone_entry 视觉歧义 → Qwen；周期系统指标 → DeepSeek）+ 有界异步优先级队列 + 复用的 libcurl HTTP 客户端（超时/有限重试/指数退避/熔断）+ 固定提示词与 schema 校验 + **API 故障绝不影响实时管道**。

### 2. 实现文件

| 文件 | 操作 | 说明 |
|---|---|---|
| `include/jetedge/llm/llm_types.h` | 新建 | LlmProvider / RequestPriority / LlmRequest / LlmResponse / CloudAnalysisRecord |
| `include/jetedge/llm/llm_config.h` | 新建 | LlmConfig（端点、队列、熔断、路由表） |
| `include/jetedge/llm/request_queue.h` | 新建 | BoundedPriorityQueue 模板：优先级（低枚举值先出）+ FIFO 决胜；满时弹出最低优先级；shutdown 唤醒 |
| `include/jetedge/llm/circuit_breaker.h` + `src/llm/circuit_breaker.cpp` | 新建 | CLOSED → OPEN（5 次连续失败）→ HALF_OPEN（30 s 恢复超时）→ CLOSED（2 次成功）或 re-OPEN，每 provider 一个实例 |
| `include/jetedge/llm/http_client.h` + `src/llm/http_client.cpp` | 新建 | libcurl easy API：连接复用（MAXCONNECTS=4）、TLS 校验、Bearer 认证、仅瞬态错误重试（连接失败/5xx，4xx 不重试）、500 ms 起步指数退避 |
| `include/jetedge/llm/prompt_manager.h` + `src/llm/prompt_manager.cpp` | 新建 | Qwen 视觉复核 / DeepSeek 指标诊断固定提示词；OpenAI-compatible 请求体（Qwen 用 base64 data URL 图像）；响应 content 抽取 + jsoncpp 字段级校验 |
| `include/jetedge/llm/llm_router.h` + `src/llm/llm_router.cpp` | 新建 | 路由决策 + worker 线程（100 ms 轮询）+ 熔断集成 + 云分析 JSONL 写出 + 统计 |
| `src/events/event_probe.cpp` | 修改 | 本地事件先写 JSONL，再非阻塞 enqueue |
| `src/pipeline/pipeline.cpp` | 修改 | LlmRouter 初始化、DeepSeek 周期指标定时器（g_timeout_add_seconds）、shutdown 顺序 |
| `src/common/config_loader.cpp` | 修改 | `llm:` YAML 段解析与范围校验 |
| `apps/jetedge_server/main.cpp` | 修改 | 启动时 `load_secrets_file()`（env 优先，`~/.jetedge/secrets.env` 兜底） |
| `configs/streams_stage7.yaml` | 新建 | llm 默认禁用；真实端点（dashscope.aliyuncs.com / api.deepseek.com）；模型 qwen3.6-flash / deepseek-v4-flash；队列 32；熔断 5/30/2；路由仅开 zone_entry→Qwen |
| `tests/test_circuit_breaker.cpp` | 新建 | 6 组熔断状态机用例 |
| `tests/test_prompt_manager.cpp` | 新建 | schema 解析单元测试（含 markdown 围栏剥离，20 项断言） |
| `CMakeLists.txt` | 修改 | 链接 jsoncpp（pkg-config）+ libcurl；test_circuit_breaker / test_prompt_manager 目标 + ctest |

线上验收修复：qwen 系列模型（qwen-vl-plus / qwen3.6-flash）把请求的 JSON 包在 ```json markdown 围栏里返回，jsoncpp 直接解析失败 → 新增 `strip_markdown_fence()`（prompt_manager）在 `validate_review_json` 解析前剥离围栏；`llm_router` 将 `result_json` 存为剥离后的规范化内容（analysis JSONL 记录可直接解析）。模型默认值随用户决策切换：qwen `qwen-vl-plus`→`qwen3.6-flash`、deepseek `deepseek-chat`→`deepseek-v4-flash`（均已在各自 API 模型列表确认存在）。

调试中修复：CURL* 为 void* 导致前向声明冲突（改 `void* curl_`）；lock_guard 无 unlock（改 unique_lock）；`<thread>` include 缺失；post_raw content_type 硬编码；PromptManager 硬编码 model 名（改 config 注入）；enqueue 后移动对象读取（先拷贝局部变量）；熔断 OPEN 日志显示 0 次失败（先捕获 reached）。

### 3. 实机验收结果（2026-08-01，全部实测）

| 检查项 | 结果 |
|---|---|
| 单元测试 | test_event_engine（回归）+ test_circuit_breaker 6 组：ALL PASS |
| llm 禁用回归 | 4 路 2072 帧 EXIT=0；事件 1194 行分布与 Stage 6 完全一致；0 条 llm 日志；无 analysis JSONL |
| mock 端点全链路 | qwen 369（全部 zone_entry）+ deepseek 6（周期 5 s）；375 行 analysis JSONL 逐行校验 0 失败；150 次 keyframe 编码 0 失败；管道 FPS 无影响（cam1 44.09 vs 44.07）|
| 优先级 shed | DeepSeek kLow 被高优先级挤占后丢弃 —— 符合设计 |
| 死端点故障注入 | curl rc=7，每请求 3 次尝试 500/1000 ms 退避；5 次失败后熔断 OPEN；288 请求被跳过；管道 2072 帧 EXIT=0 |
| 密钥安全 | 日志只含 LLM010 错误码，无任何 key/base64 泄漏 |
| 线上真实 API（第一轮 16:52，修复前）| deepseek-chat 成功 1 次（4791 ms, http 200）；qwen-vl-plus 5 次 http 200 但全部 schema 解析失败 → 熔断 OPEN → 288 请求跳过（费用受控）；管道 EXIT=0 不受影响 |
| 线上真实 API（最终 18:32，修复后）| qwen3.6-flash 成功 2 次（4381 / 9042 ms, http 200），result 记录逐行可直接解析；0 条解析失败/熔断日志；1442 帧 EXIT=0；事件 1172 行与 Stage 6 一致；deepseek-v4-flash 单独真实调用验证通过 |
| 单元测试（最终）| test_event_engine + test_circuit_breaker + test_prompt_manager（20 项）全部 ALL PASS |

### 4. 验收状态

**验收通过（2026-08-01）。** 线上真实 API 受限测试完成：首轮暴露 Qwen markdown 围栏解析缺陷（根因经单次真实 curl 确认），修复（围栏剥离 + 规范化存储）并切换模型（qwen3.6-flash / deepseek-v4-flash）后复验通过；管道全程不受 API 行为影响。已按 CLAUDE.md 4.6 流程同步文档并提交。

### 5. 遗留

- 进程退出时在途云端请求被丢弃（best-effort 设计）；
- 未做图片去重/缩放（重复关键帧与 1280x720 原图直发，属后续优化）；
- 响应校验为 jsoncpp 字段级，非完整 JSON Schema；
- 2 小时稳定性测试属最终验收项。

详细报告：`docs/stage7_llm.md`。

---

## 2026-08-01 Stage 8：RTSP 故障隔离与恢复（实施中，未验收）

### 1. 目标

把 SourceManager 扩展为支持 RTSP 源，实现每流独立状态机（OFFLINE → CONNECTING → RUNNING → DEGRADED → RECONNECTING → FAILED）、指数退避重连、单流故障隔离（坏流不影响健康流）、恢复后输入 FPS 验证、超过重试阈值停止重试风暴。不重启整个进程作为恢复设计。调度器（NORMAL/PRESSURE/THERMAL/CRITICAL/RECOVERY）属于后续阶段，不在 Stage 8 范围。

### 2. 测试环境（已就位，未启动验证）

| 项 | 状态 | 说明 |
|---|---|---|
| MediaMTX v1.19.3（linux arm64）| 已下载 | 单文件二进制在 `~/jetedge-rtsp/mediamtx`（用户目录，无 sudo/系统包，不入 Git）|
| `~/jetedge-rtsp/mediamtx.yml` | 已创建 | rtspAddress :8554，paths cam1..cam4 |
| `scripts/rtsp_serve.sh` | 已创建 | server-start/stop、cam-start/stop/restart、all-start/stop、status；ffmpeg `-re -stream_loop -1 -c copy` 把 DeepStream 样例视频循环推成 rtsp://127.0.0.1:8554/camN（cam1=sample_720p.h264，cam2=sample_office.mp4，cam3=sample_walk.mov，cam4=sample_ride_bike.mov）|

### 3. 代码实现（已写入，已编译，未实机验收）

| 文件 | 操作 | 说明 |
|---|---|---|
| `include/jetedge/pipeline/reconnect_policy.h` + `src/pipeline/reconnect_policy.cpp` | 新建 | 纯逻辑每流重连状态机（无 GStreamer 依赖）：状态转换、指数退避 base×2ⁿ 上限 15 s、连续失败 5 次进 FAILED 停止自动重试、成功重置计数 |
| `tests/test_reconnect_policy.cpp` | 新建 | 状态转换/退避序列/FAILED 阈值/成功重置 等 7 组用例（编译通过，1 处测试断言错误已修正：base=1000/max=1500 首次退避应为 1000 而非 1500，实现符合"base×2ⁿ 封顶 max"文档语义）|
| `include/jetedge/pipeline/stream_config.h` | 修改 | 新增 `RtspConfig`（enable/live_source/watch_timeout_sec/max_retries/backoff_base_ms/backoff_max_ms/verify_sec/min_fps/rtspsrc_latency_ms/**transport: tcp\|udp\|auto**）|
| `src/common/config_loader.cpp` | 修改 | 新增 `rtsp:` YAML 段解析 + 范围校验（含 transport 白名单）；`streams[].type` 仅允许 file/rtsp，rtsp 类型要求 `rtsp.enable: true` |
| `include/jetedge/pipeline/source_bin.h` + `src/pipeline/source_bin.cpp` | 修改 | rtsp 分支：rtspsrc（latency、drop-on-latency、transport 属性）动态 pad 按 caps 选 H.264 → h264parse → nvv4l2decoder；`teardown()`（probe → NULL → 移除，可重建）；`sync_state_with_parent()`（运行时重建后同步状态）；frame probe 记录 last_frame_ts_ms |
| `include/jetedge/pipeline/source_manager.h` + `src/pipeline/source_manager.cpp` | 修改 | 接线 `RtspConfig`；`rebuild_source(idx)`：unlink decoder pad → release streammux request pad → teardown → build 新链 → 重请求同名字 `sink_<idx>`（stream_id→pad index 映射稳定）→ link → sync；live-source 按 `rtsp.enable && live_source`；`frame_count/last_frame_ts_ms/is_rtsp_source` 辅助 |
| `include/jetedge/pipeline/pipeline.h` + `src/pipeline/pipeline.cpp` | 修改 | `RtspConfig` 参数；每流 `RtspWatch`（policy + 时间戳）；1 s watchdog 定时器（断流检测→DEGRADED→退避 deadline→重建；CONNECTING 首帧后开 verify_sec 窗口验证 FPS≥min_fps 才 RUNNING；FAILED 停止自动重试）；bus ERROR 按元素归属分流（`src-<id>-*` 前缀向上遍历父链→流级重连，其余致命退出）；重连后重装 EOS probe；周期报告加 rtsp 状态（state/age/reconnects/failures）；RTSP 模式跳过 PAUSED preroll（防启动失败整链重置 NULL）；重建后 pipeline 非 PLAYING 时自愈重拉 |
| `apps/jetedge_server/main.cpp` | 修改 | `pipeline.build(...)` 传 `config.rtsp` |
| `CMakeLists.txt` | 修改 | 接入 `reconnect_policy.cpp` + `test_reconnect_policy`（ctest）|
| `configs/streams_stage8.yaml` | 修改 | 加 `transport: tcp`（实测结论，见下）|
| `scripts/rtsp_serve.sh` | 修改 | 三处修复（见 §5 测试环境排障）|

### 4. 编译与单元测试（已通过）

- `cmake -S . -B build && cmake --build build -j2`：全部目标编译通过，无警告
- `ctest` 4/4 PASS：test_event_engine + test_circuit_breaker + test_prompt_manager + **test_reconnect_policy（37 checks）**
- file 模式回归（`streams_stage7.yaml` 副本输出到独立路径）：4 路 2072 帧 EXIT=0；事件 JSONL **1194 行与 Stage 6/7 完全一致**（cam1 appearance 369 / disappearance 369 / count_high 33 / count_exit 32 / zone_entry 369）；0 条 rtsp/llm 日志 → 无行为退化

### 5. RTSP 测试环境排障（重要，全部实机定位）

| # | 问题 | 根因 | 修复 |
|---|---|---|---|
| 1 | `-c copy` 从 mp4/mov 推 RTSP 的流损坏（客户端 "Invalid level prefix"，服务器 "invalid FU-A packet"）| mp4/mov 的 H.264 是 AVCC（长度前缀）格式，RTP 需要 Annex-B | 发布命令加 `-bsf:v h264_mp4toannexb` |
| 2 | MediaMTX 收 4 路 UDP 发布大量丢包（FU-A 碎片中途丢失）| UDP socket 缓冲不足（无 CAP_NET_ADMIN，rmem 受限）| 发布改 TCP：`-rtsp_transport tcp`（注意：该选项是输出端 muxer 选项，必须放在 `-i` 之后，否则 "Option rtsp_transport not found"）|
| 3 | gst 客户端 UDP 收 1080p 必丢包（gstudpsrc 要 512 KB，系统 `net.core.rmem_max`=212 KB 且无权限调大）| 系统级 UDP 缓冲限制（不动系统网络设置）| 应用侧 rtspsrc 走 TCP：新增 `rtsp.transport: tcp` 配置（SourceBin 设 `protocols=GST_RTSP_LOWER_TRANS_TCP`）|
| 4 | cam1 发布进程循环一次后死亡：`sample_720p.h264: Operation not permitted` | **ffmpeg 对 raw .h264 + `-stream_loop -1` 的缺陷**：循环边界 seek 回 0 报 EPERM（在 /tmp 副本上复现，与文件系统/权限无关；mp4/mov 循环正常）| cam1 改用同场景 `sample_720p.mp4` 发布 |

### 6. 当前状态与未完成项

- [x] CMakeLists 接入 reconnect_policy + test_reconnect_policy
- [x] SourceManager 重连管理（release/重建 request pad、故障隔离、FPS 验证窗口）
- [x] Pipeline bus 错误分流 + 断流 watch 定时器 + 周期状态报告
- [x] 编译 + 单元测试运行 + file 模式回归
- [x] RTSP 环境启动验证（四路可探测、cam1/cam2 客户端解码 0 错误）
- [ ] **cam3/cam4（mov 容器）发布后 RTP 仍损坏**：客户端 TCP 消费持续 11~19 错误/10 s；MediaMTX 侧零错误；抓包落盘本地解码仍损坏 → 损坏在发布端。cam1(mp4)/cam2(mp4) 干净。**假设：ffmpeg mov→RTP 打包问题。下一个实验（已准备未执行）：`ffmpeg -i sample_walk.mov -c copy sample_walk_remux.mp4` remux 成 mp4 再发布验证；若通过则 rtsp_serve.sh 全部改用 mp4 源**
- [ ] 应用级 RTSP 冒烟（已验证错误分流/重连调度正确：所有流级错误被识别并调度重连、进程不崩；**四路尚未进入 RUNNING**，待流修复后重跑）
- [ ] 实机故障注入验收（停 cam3 其余三路继续、恢复自动接入、反复故障 ≥10 次、pad 无泄漏、FAILED 停止重试风暴、内存收敛、EOS/Ctrl-C 干净）
- [ ] `docs/stage8_rtsp.md` 报告 + README/PROGRESS 同步 + 提交

**当前状态：实现中（implemented，编译/单测/回归通过；实机验收被 cam3/cam4 mov 发布问题阻塞）**——不视为完成。

## 2026-08-02 Stage 8：RTSP 实机排障（崩溃修复 + 根因定位，未验收）

### 1. 会话起点状态确认

- 代码/单测/回归状态不变：ctest 4/4 PASS、file 模式回归 1194 事件行与 Stage 6/7 一致
- cam3/cam4 remux 修复生效：四路 ffmpeg 客户端逐一验证 **0 错误 / 10 s**（此前 cam3/cam4 11~19 错误/10 s）

### 2. 崩溃修复：`tick_rtsp_watch` 悬垂引用（use-after-free）

| 项 | 内容 |
|---|---|
| 症状 | 4 路 RTSP 运行 ~30 s 段错误（SIGSEGV，exit 139，core 走 apport）|
| 定位 | gdb 栈：`tick_rtsp_watch` → 日志 `"rtsp stream=%s verified…"` 在 `__strlen` 崩溃 |
| 根因 | `SourceManager::stream_ids()` 按值返回临时 `vector<string>`；`const std::string& sid = stream_ids()[i]` 绑定临时元素，语句结束即悬垂；后续 `sid.c_str()` use-after-free（依赖堆复用，故偶发，首轮 90 s 崩溃、gdb 复现延迟到 cam4 验证消息）|
| 修复 | `src/pipeline/pipeline.cpp:612` 改为按值拷贝 + 注释；已 grep 全部调用点，其余均为按值拷贝或单表达式内使用，无同类问题 |

### 3. 发布流健康验证（实机，全部干净）

- 四路 ffmpeg 客户端 0 错误/10 s
- 纯 gst 客户端（`rtspsrc → h264parse → nvv4l2decoder`，与 app 同链）消费 cam2 40 s 跨 7+ 个循环边界：0 错误
- ffprobe 抓 cam1 RTP PTS 55 s（跨 48 s 循环边界）：**无 >1 s gap、无大 PTS 回跳**，~77 pkt/s 连续
- 结论：**发布端干净，问题在应用侧**（MediaMTX 日志证实每个会话服务端都在发送 "is reading"，而应用 frame probe 为 0）

### 4. 应用侧两个根因

**根因 A（cam1 永不进 RUNNING）：长 GOP 与首帧窗口不匹配**
- `sample_720p.mp4` GOP **8.33 s**（ffprobe 实测 keyframe 间距）；cam2/3/4 源 GOP 4.3~9.6 s 但实测首帧 <5 s 到达
- `watch_timeout_sec=5` 同时充当"连接后首帧等待窗口"：连接落在 GOP 中段时，首个 IDR 最迟 8.33 s 才到 → 5 s 到点拆连接 → 新连接又落在新 GOP 中段 → 反复 `no-frames`（单路运行 4 次连接才撞上一次成功；4 路运行时 cam1 4 个会话全部失败）
- 附证：单路 debug 运行（GST_DEBUG）显示健康会话里 depay 从 2.4 s 起 30 fps 推帧、h264parse "Inserting AUD" 逐帧、decoder 正常出帧——失败/成功取决于连接时刻离下一个 IDR 是否 <5 s

**根因 B（cam2/3/4 同时 stall + 误 FAILED）：重建 churn 扰动 muxer + 失败信号重复计数**
- cam1 每 ~7 s 重建一次；10:46:33 cam1 重建与 cam2/3/4 同时刻 stall（三条 DEGRADED 与 "cam1 rebuilt" 同一秒）→ 单流重建 churn 扰动 live-source nvstreammux，其他流 decoder 出帧停止（连接保持存活）
- app 对 stall 流执行 teardown 重建时，垂死 rtspsrc 连发多条 bus ERROR（"Internal data stream error" + "Could not write to resource"，2~4 条/次），**每条都计一次 failure** → 单次真实故障吞掉 2-3 次重试预算 → cam2/cam4 在 6~7"次"失败即 FAILED（日志 298-301 行）
- 附带观察：churn 期 per-stream fps 降至 ~15（半速），恢复后正常

### 5. 修复（已编译通过，未回归）

| Fix | 内容 | 文件 |
|---|---|---|
| A | 新增 `rtsp.first_frame_timeout_sec`（默认 12 s，校验 [1,120]）：首帧等待窗口与运行中 stall 窗口（`watch_timeout_sec=5`）分离 | `stream_config.h` / `config_loader.cpp` / `pipeline.cpp` / 两个 stage8 配置 |
| B | `schedule_reconnect` 失败信号合并：已 FAILED 或已有 pending 重连（`now < deadline_ms`）时忽略新失败信号，不再重复计数 | `pipeline.cpp` |
| C | 自愈日志改用 `gst_element_state_change_return_get_name`（原 `rc=0` 实为 `GST_STATE_CHANGE_FAILURE=0`，易误读为成功）| `pipeline.cpp` |

### 6. 未完成（下次会话接续）

- [ ] ctest 4/4 回归（Fix 后未跑）
- [ ] 4 路 120 s 冒烟：四路应全部进 RUNNING（发布端自 10:39 持续运行；建议先 `cam-restart` 再跑）
- [ ] 实机故障注入验收（停 cam3 其余三路继续、恢复自动接入、反复故障 ≥10 次、pad 无泄漏、FAILED 停止重试风暴、内存收敛、EOS/Ctrl-C）
- [ ] `docs/stage8_rtsp.md` + README/PROGRESS 同步 + 提交

**当前状态：实现中（implemented，编译通过；冒烟与故障注入验收未跑，不视为完成）**

## 2026-08-02 Stage 8：验收通过（下溢假 stall 与陈旧错误计数两个缺陷定位修复）

### 1. 会话起点

- 上一会话遗留：Fix A/B/C 已编译未回归；4 路冒烟与故障注入未跑
- 本会话完成：回归 → 冒烟 → 10 轮故障注入 → FAILED 路径 → 文档同步提交

### 2. 回归与冒烟（全部实测）

- ctest 4/4 PASS（Fix A/B/C 后）；file 模式回归 1194 事件行与 Stage 6/7 一致
- 4 路 RTSP 冒烟 200s：四路 ~10s 内 RUNNING（修复 A 生效——此前 cam1 永不进 RUNNING），0 重连 0 失败，每路 ~29.3 fps，事件 6773 行 0 非法，SIGINT 干净

### 3. 缺陷 R1：watchdog tick 时间戳下溢 → cam4 每轮必假 stall

- 症状：10 轮 cam3 停/恢复中 cam4 **每一轮**都在 cam3 断源后 1-2s 报 DEGRADED（cam1/cam2 从不误报），随后无谓重建
- 证据链（三层实机定位）：
  1. 纯 GStreamer 客户端（无 muxer/nvinfer，与 app 同源链）消费 cam4：停 cam3 期间 NAL 速率恒定 ~630/2s 零跌落 → **服务器持续投递，问题在应用管道**
  2. `GST_DEBUG=nvstreammux:5`：断源期间 muxer 推 batch size=3 部分批次，source 3 帧 stall 窗口内 60 帧/2s 无跌落 → **decoder 全程输出 → stall 是假的**
  3. 插桩 tick 日志实锤：`last=10957559 now=10957523`（**last 比 now 大 36ms**）→ `now - last` 无符号下溢 → 巨数 → 假 stall
- 根因：`tick_rtsp_watch` 循环开头捕获一次 `now`；循环体内前一流（cam3）的 `do_reconnect` 重建（teardown/build/link/sync）耗时 ~100ms 主线程重活；cam4 的检查在重建之后执行——`now` 是重建前的旧值、`last`（帧探针）是重建后的新值 → 下溢。cam4 是 cam3 后检查的第一个流所以每轮必中；cam1/2 在循环前段先于重建检查从不误报
- 修复：`now` 移入每流循环内重新读取 + kRunning 检查加 `last <= now` 保护（探针/tick 良性竞态一并覆盖）
- 验证：修复后同样 10 轮注入 **cam4 0 stall / 0 reconnect / 0 failure**；此前"muxer 扰动 cam4"现象全部消失（假 stall 触发的重建 churn 才是真实扰动源）

### 4. 缺陷 R2：垂死元素错误重复计数 → 健康流误 FAILED

- 症状（修复前）：cam4 帧流健康（RECONNECTING 期间 frames 持续增长）却累进 6 次失败 FAILED；单次真实故障吞掉 2-4 次重试预算
- 根因：teardown 中旧 rtspsrc 连发 2~4 条 bus ERROR（"Internal data stream error"/"Could not write to resource"），消息异步到达；重建（mark_connect）后到达的错误不在 Fix B 的 pending 窗口 → 每条计为新失败
- 修复：`on_bus_message` 流级错误加**元素身份校验**（`SourceBin::is_chain_element`：错误源必须是当前链实例，旧元素一律忽略并记 INFO 日志）
- 验证：早期运行 19 次正确忽略；真实 404（"Could not open resource"/"Not found"，发布端重启竞态）仍正确计数并退避重试恢复

### 5. 实机故障注入验收（修复后，PID 55118，8.5 分钟）

| 检查项 | 结果 |
|---|---|
| 10 轮 cam3 停 6s/恢复 | cam3 每轮 stall→1 失败→恢复→failures 归零（10/10）；恢复期偶发真实 404（发布端未就绪）正确计数后恢复 |
| cam1 / cam2 / cam4 | **全程 0 stall / 0 reconnect / 0 failure**（460s，~30fps）|
| Phase 2 长期停源 | 6 次真实连续失败 → RTSP006 FAILED → 0 次后续重试（风暴停止）；发布端恢复后保持 FAILED（按设计）|
| 事件 JSONL | 8419 行逐行校验 0 非法（cam1 8045 / cam2 118 / cam3 82 / cam4 174）|
| RSS | 616.4 → 650.5 MiB 收敛 |
| SIGINT | 优雅退出 exit OK（含 FAILED 态流）|
| ctest | 4/4 PASS |

### 6. 验收结论

**验收通过（2026-08-02）。** 已按 CLAUDE.md 4.6 流程同步文档（`docs/stage8_rtsp.md` 新建、README/PROGRESS/日志更新）并提交推送。

### 7. 遗留（后续阶段）

- FAILED 不自动复活（设计如此，重试预算耗尽）；2 小时稳定性测试属最终验收项
- 确定性 C++ 动态调度器（NORMAL|PRESSURE|THERMAL|CRITICAL|RECOVERY）+ 自适应推理间隔 = Stage 9

## Stage 9：确定性 C++ 动态调度器（NORMAL | PRESSURE | THERMAL | CRITICAL | RECOVERY）

**日期**：2026-08-02。验收通过，详见 `docs/stage9_scheduler.md`。

### 1. 交付物

- `scheduler_policy`：纯逻辑状态机（滞回 enter>exit、min_hold、cooldown、调整预算 2/120s、热优先级、CRITICAL 不增载、缺失指标不困死）；状态表 NORMAL{0,0,0} / PRESSURE{0,1,2} / THERMAL{0,2,3} / CRITICAL{1,3,15} / RECOVERY 三级逐级恢复
- `system_metrics`：只读采样 /proc/stat + /proc/meminfo + thermal_zone*/temp（最大可读温度 + zone 名；cv0-2 缺失跳过）
- SourceBin decoder src 探针逐流间隔 drop（计数先于丢弃 → RTSP watchdog 看全速率；drop_counter 重建归零 → 首帧必保留）
- `scheduler:` YAML 段全字段校验；Pipeline 2s tick 驱动 + 结构化日志 + 周期报告

### 2. 单测

`test_scheduler_policy` 12 组 54 checks ALL PASS；ctest 5/5。

### 3. 实机验收摘要（4 路 RTSP）

| Run | 场景 | 结果 |
|---|---|---|
| A | 正常负载 100s | 全程 NORMAL [0 0 0]，0 重连，cpu 29-37% / temp 53-57°C，无回归 |
| B | 6×yes 烧机 65s | cpu 99.9% → PRESSURE [0 1 2]（cam2/3=1、cam4=2、cam1 high=0 受保护）；实测帧率 cam2 15.0 / cam4 10.0 fps（=30/2、30/3 精确）；停烧机 → RECOVERY 三级（[0 1 2]→[0 0 1]→[0 0 0]）→ NORMAL；全程 0 失败 |
| C | debug 低阈值（真实温度）| 53.8°C → THERMAL [0 2 3]；56.1°C → CRITICAL [1 3 15]；预算 2/2 封顶；cpu 33→25%（降载生效）；cam4 ~2fps 运行 0 假 stall |
| D | 阈值 56/55.6 闭环 | CRITICAL⇄RECOVERY 闭环机制验证；发现调参规则：**滞回间隙须 > 热噪声（~0.5°C）**，0.4°C 间隙导致边界抖动（每次迁移本身正确）|

JSONL 逐行校验 0 非法（events 2287+2868、detections 43123+48795）；RSS 631.9→633.1 MB 收敛；全部 SIGINT 干净退出。

### 4. 本会话修复

- kNormal 中 CRITICAL 进入被 cooldown 误挡（安全迁移须无条件放行）→ 修复 + 测试
- 间隔变更日志旧值=新值（赋值先于日志）→ 调整顺序

### 5. 遗留

- CRITICAL 低优先级为有界间隔 15（非完全停帧；live mux 停帧行为未实机验证）
- 本机主动散热使负载下温度稳定 ~55.5°C，无法在不停管道前提下制造 >1°C 降温摆幅 → CRITICAL→RECOVERY 稳态单次恢复由单测 + Run B 同路径覆盖
- 2 小时稳定性 / GPU 专项压力测试属最终验收项；下一阶段 ftrace / CPU Affinity（README 路线）

## Stage 10：ftrace / CPU Affinity 性能分析

**日期**：2026-08-04。验收通过，详见 `docs/stage10_ftrace.md`。

### 1. 交付物

- 60s sched ftrace 基线（before，无钉核）：70 线程自由漂移 6 核，迁移率 8-39%；wake→run（next_pid 配对）尾部延迟真实存在（rtpjitterbuffer p99 45.3ms、nvstreammux task0 p99 26.0ms）
- 实验 1（keep）：8 个解码线程（4× src-camN-decode + 4× V4L2_DecThread）每流一对钉核 camN→cpuN-1 → 迁移率全 0，解码路径尾部 p99 45.3→1.48ms、26.0→2.24ms，端到端 29.3-29.7→29.6-29.8 fps 零回归
- 实验 2（revert）：12 个应用线程聚堆钉 cpu4-5 → 内部竞争集中（20512 p99 161ms，cpu4-5 窗口全是被钉同类线程 + containerd 互抢），逐事件核对根因后撤销；应用线程为旁路任务不影响主链
- `scripts/start_pipeline.sh`（start/stop/status）：启动管道 → 轮询检测 decode 线程 → 自动钉核（DeepStream 内部线程无法代码级 setaffinity，固化到脚本）

### 2. 实机验收

- 脚本全流程重启验证：cam1→cpu0、cam2→cpu1、cam3→cpu2、cam4→cpu3 每核 1 解码对，4 路 RUNNING，in=infer=out 无损
- 3 份 60s trace（~21MB/份，/tmp 未入库）逐事件核对尾部延迟真实性，排除 wakeup→switch 误配对（首版配对含运行时间 100-400ms 假尾，改用 next_pid 配对剔除）

### 3. 遗留

- V4L2 与 src-camN 的流级配对按出现顺序轮转近似（未做唤醒关联精确配对）
- PRESSURE/THERMAL 负载下钉核与调度器交互未测
- 未做 trace_marker 应用级打点（sched 事件已足以下结论）；阶段时间戳留作后续
