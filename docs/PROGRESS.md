# JetEdge-Agent Progress

最后更新时间：2026-08-01

## 当前结论

- 阶段 0（环境核查）：已完成 ✓
- 阶段 1（单路硬件解码）：已完成 ✓
- 阶段 3（YOLO11s ONNX 导出、验证、传输与 SHA256 一致性验收）：已完成 ✓
- 旧方案阶段 2（四路 streammux + fakesink）：已完成 ✓，代码已在新方案 Stage 5 中复用
- 阶段 4（TensorRT FP16 Engine + 单路 nvinfer 验证）：已完成 ✓（2026-08-01）
- **阶段 5（四路检测 + Tracker + 结构化 JSONL + per-stream Metrics）：已完成 ✓（2026-08-01）**
- **阶段 6（事件系统、事件去重和关键帧抽取）：已完成 ✓（2026-08-01）**

> **阶段编号变更**：旧 `implementation_plan.md` 的阶段 2（四路 streammux）和阶段 3（TensorRT+Tracker+四路检测）已被 `README.md` 新方案重新组织。新方案 Stage 4 只做单路 TensorRT+nvinfer（不含 Tracker），四路检测和 Tracker 归入 Stage 5。

## 阶段 3 验收记录

- Windows 本机 Python 版本：3.11.9
- 模型：YOLO11s
- ONNX 输入：1x3x384x640
- ONNX 输出：1x84x5040
- Batch：1
- Dynamic shape：false
- ONNX opset：12
- ONNX Checker：PASSED
- ONNX Runtime inference：PASSED
- NaN/Inf check：PASSED
- SHA256：41abd2ff906712b41c60de9b7d5d5f09918e23a331d80cc0926071600fd3e078
- Windows 与 Jetson SHA256：一致
- Jetson 模型路径：`/home/seeed/JetEdge-Agent/models/yolo11s.onnx`

## 文件同步策略

- GitHub 只同步源码、脚本、配置模板、Markdown 文档和 `models/model_info.txt`。
- 模型权重、ONNX、TensorRT Engine、视频、密钥和大日志不进入 Git。
- 大文件通过 SCP/rsync 双通道同步，并使用 SHA256 做端到端验收。

## 阶段 4 验收记录

- FP16 Engine：`yolo11s_b1_384x640_fp16.engine`（22,866,804 B ≈ 21.81 MiB，SHA256 `c6cc41d0...a82274a`，无 warning，构建耗时 365s）
- 自定义 parser：`src/inference/yolo11_parser.cpp` → `build/libnvds_yolo11_parser.so`（输出实测为绝对像素坐标 + 已 sigmoid 的 class scores）
- 单路 720p 验证：1440 帧全部处理，每帧 8-16 个目标，bus conf=0.95 / car conf=0.94（与 Python ground truth 吻合）
- EOS、Ctrl-C 优雅退出，RSS 稳定（306.6 → 307.4 MiB）
- 关键修复：`net-scale-factor=1/255`（0-255 输入导致检测错乱）、DeepStream 传 2 维 dims、相对路径解析

## 阶段 5 验收记录（2026-08-01）

### 模型与 Engine

- 派生 batch-dynamic ONNX：`yolo11s_dynamic.onnx`（37,944,131 B，SHA256 `fa27873a...b766e48`，由已验收 ONNX 派生，权重不变，输入/输出 batch 维符号化 + 6 个 hard-coded batch=1 Reshape 常量改为 batch-relative；脚本 `scripts/make_batch_dynamic_onnx.py`）
- 派生 ONNX 验证（ORT 1.23.2 CPU）：batch=1 输出与原 ONNX 完全一致（max diff 0.0）；batch=4 推理 PASSED（输出 4x84x5040）
- FP16 Engine：`yolo11s_b4_384x640_fp16.engine`（22,278,268 B ≈ 21.25 MiB，SHA256 `136bd5fd...b06818d`，构建耗时 494.2 s，仅 1 条 DLA-fallback warning）
- Engine profile：images MIN=1x3x384x640 OPT=4x3x384x640 MAX=4x3x384x640；binding images 4x3x384x640 → output0 4x84x5040（opt）

### 代码与配置

- nvtracker 集成（NvDCF_perf，ll-config 用 DeepStream 样例配置），pipeline：streammux → nvinfer → nvtracker → fakesink
- 结构化 JSONL 输出：`{"ts_ms","stream_id","frame_num","track_id","class_id","class","confidence","bbox":[l,t,w,h]}` → `logs/stage5_detections.jsonl`
- per-stream metrics：input（nvinfer sink）/ inference（nvinfer src）/ output（nvtracker src）三阶段 FPS + 每帧检测数 + 周期报告（5s）
- 配置：`configs/streams_stage5.yaml`（4 路不同视频）、`configs/nvinfer_yolo11s_b4_fp16.txt`（batch-size=4）

### 实测结果

| 检查项 | 结果 |
|---|---|
| 4 路不同视频（720p bus/car、office、walk、ride_bike）| 2072 帧全部处理，cam1 1442 帧 / cam2 163 / cam3 288 / cam4 179，EXIT=0 |
| stream_id 映射 | cam1 car/bus/truck、cam2 person、cam3 person、cam4 bicycle/skateboard/backpack（与各场景匹配）|
| 4 路同一视频 | 4×1442 帧，每路 17248 检测、obj/frame=11.96 完全一致（batch 偏移正确）|
| 每流 FPS（4 路同视频）| in=infer=out=52.84 fps/流（满 batch，总吞吐 ~211 fps）|
| track_id 跨帧稳定 | cam1 track=60 连续 701 帧无断档；cam2 track=0/1 连续 161 帧 |
| bbox 坐标还原 | bus [235,1,404,382]@640x384 → [469.05,1.96,805.56,716.23]@1280x720（×2.0 / ×1.875，误差 <1px）|
| 置信度 | bus conf=0.955（Stage 4: 0.95）、car conf 0.58-0.97，与 ground truth 吻合 |
| JSONL | 18,333 行（4 路不同视频），每行含 stream_id/track_id/class/confidence/bbox |
| EOS | 4 路 EOS 分别处理（`Successfully handled EOS for source_id=0..3`）+ 总 EOS 优雅退出 |
| Ctrl-C | SIGINT → 优雅退出，EXIT=0 |
| 内存 | 3 次连续运行 RSS 起始 610-611 MB、15s 收敛 ~615 MB；跨运行无残留增长（无泄漏）|
| dynamic engine batch 不满 | 短流先 EOS 后 batch 不满，nvinfer 按实际 batch 推理，无报错 |

### 遗留说明

- 4 路不同视频运行时长由最短视频 + cam1 决定（本配置 ~33 s）；长时间稳定性测试（2 小时）属于最终验收项，尚未执行
- 坐标空间为 mux 输出 1280x720；nvinfer 对 720p 输入做非等比拉伸到 640x384 推理，bbox 精确映射回 720p 空间（已数值验证）
- JSONL 每行记录 tracker 之后的 obj_meta（confidence 保留检测置信度，track_id 由 NvDCF 分配）
- Stage 6 事件系统未开始

## 阶段 6 验收记录（2026-08-01）

### 事件引擎与关键帧

- 事件系统:appearance / disappearance(grace=15 帧)/ count_high(threshold=3, 滞回=1)/ count_exit / zone_entry(road [0,250,1280,470] @ cam1)
- 事件 JSONL:1194 行,逐行 JSON 校验 **0 失败**;字段 ts_ms/stream_id/frame_num/event/class_id/class/track_id/confidence/bbox/count/zone/keyframe
- 事件分布:cam1 appearance 369 / disappearance 369 / count_high 33 / count_exit 32 / zone_entry 369;cam2/3/4 少量
- 关键帧:150 次保存成功(cap=150),113 个唯一文件(同毫秒多事件同名覆盖,属已知口径),**0 错误**
- 关键帧内容:cam1 keyframe vs 源视频同帧 SSIM=0.985;cam2 vs office 0.976 / vs bus/car -0.028(stream 映射精确)

### 关键帧取帧技术路线(核心攻关)

1. gst_buffer_map 直读像素 → 失败:NVMM buffer map 出的是 `NvBufSurface*`(64 B)而非像素(deepstream-test4 模式确认);
2. NvBufSurfaceMap CPU 映射 + NvBufSurface2Raw 兜底 → 部分成功:PITCH surface 可 CPU map,但 batch 内 layout 混合,BLOCK_LINEAR 的 `NvBufSurface2Raw` UV plane 复制失败;
3. **官方 nvds_obj_enc(isFrame=1)** → 验收通过:`libnvds_batch_jpegenc` GPU 编码任意 layout 的整帧为 JPEG,输出经 NVDS_CROP_IMAGE_META 读回,写文件。surface 索引语义使用 `frame_meta->batch_id`(nvdsmeta.h 文档化)。

### 修复的 bug

- 事件 JSONL `keyframe`/`zone` 字段裸值无引号 → 全部行非法 JSON(上一会话仅数行数漏检,本次逐行 json 校验暴露);
- keyframe 初版对 NVMM buffer 的像素误读(KFW011 buffer too small size=64)。

### 验收结果摘要

| 检查项 | 结果 |
|---|---|
| 4 路不同视频 2072 帧 | EXIT=0,cam1 1442 / cam2 163 / cam3 288 / cam4 179 |
| 事件 JSONL | 1194 行全部合法 JSON |
| 关键帧 | 150 次保存、0 错误、内容 SSIM 0.985 验证 |
| EOS / Ctrl-C | 每流 flush + 优雅退出 EXIT=0 |
| 内存 | RSS 619.8 → 628.0 MB 收敛,无持续增长 |
| 单元测试 | test_event_engine ALL PASS |

详细报告:`docs/stage6_events.md`。

## 后续阶段

- Stage 7：Qwen + DeepSeek API、异步队列和降级策略
- RTSP 故障恢复与动态调度
- ftrace 和 CPU Affinity 分析
- INT8 PTQ 与精度回归
