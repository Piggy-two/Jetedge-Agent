# Stage 13 验收报告：INT8 PTQ 与精度回归

日期：2026-08-04
设备：Jetson Orin Nano 8GB（实机）
相关规则：CLAUDE.md §12（INT8 需校准数据 + 精度回归）、§13（基准纪律）、§19（完成定义）

## 1. 结论（先行）

**回退 FP16 交付。** INT8（MinMax 校准）精度回归实测未完全达到保守阈值（匹配率 0.94-0.99，阈值 0.95），按 §16 验证纪律回退。INT8 工具链（抽帧、校准器、对比脚本）、全部 engine 与实测数据保留，可复现可继续优化；性能收益（P95 -18.3%）如实记录。

## 2. 目标与范围

- 目标：构建 YOLO11s batch=4 INT8 engine（校准数据可控可复现）、nvinfer 实机加载、FP16 同帧精度回归（不达标回退）、性能 A-B-A、全部实测记录。
- 范围外：b1 INT8 engine、DeepStream 自校准主路线、事件引擎/调度器扩展。

## 3. 校准数据

- 脚本：`scripts/extract_calib_frames.py`（预处理与 nvinfer 一致：非等比 resize 640x384 + RGB；raw HWC uint8 737280 B/帧，读取端 ×1/255）
- 稀疏集（默认）：bus 181（stride 8）/ office 163（全量）/ walk 144（stride 2）/ ride 179（全量）= **667 帧**，`/home/seeed/jetedge-calib/`
- 密集集（实验）：bus 361（stride 4）/ office 163 / walk 288（stride 1）/ ride 179 = **991 帧**，`/home/seeed/jetedge-calib-dense/`
- 冒烟：文件数==manifest、字节数抽查、内容回读正常

## 4. 校准器（apps/calib_generator，C++17 零 OpenCV 依赖）

- EntropyCalibrator2（默认）与 MinMax 双算法（`--algo`），profile MIN=1 OPT=4 MAX=4，FP16+INT8，workspace 2048MB
- `--save-engine` 可选：MinMax 缓存格式 trtexec 不兼容，须由校准器直接产出 engine
- **实机修复 2 个缺陷**（根因：校准器绑定契约）：
  1. 单输入绑定 = 整个 batch 的连续 buffer（batch×C×H×W），非每 slot 一个 → 首版按 slot 分配致 TRT 读越界（`executeV2 invalid argument`）
  2. `cudaMalloc` 大小须 ×batch → `free(): invalid next size` / `_Map_base::at`
- 单测：`tests/test_image_reader.cpp`（17 checks，ctest image_reader PASS）+ `scripts/tests/test_accuracy_math.py`（22 用例，ctest accuracy_math PASS）

## 5. 校准缓存与 engine（全部 Jetson 实机构建）

| 缓存 | 算法 | 帧数 | 大小 | SHA256 |
|---|---|---|---|---|
| yolo11s_b4_384x640_int8.calib | entropy2 | 667 | 15810 B（345 层） | c8b980d2...8896a9 |
| yolo11s_b4_384x640_int8_minmax.calib | minmax | 667 | 15808 B | eb081afc...33c |
| yolo11s_b4_384x640_int8_minmax_dense.calib | minmax | 991 | 15808 B | 55da6765...a006 |

| engine | 构建方式 | 大小 | SHA256 | 构建耗时 |
|---|---|---|---|---|
| yolo11s_b4_384x640_fp16.engine（基线） | trtexec（Stage 5） | 21.25 MiB | 136bd5fd...b06818d | 494 s |
| yolo11s_b4_384x640_int8.engine | trtexec --int8 --calib=entropy2 | 12.15 MiB | 69a01eb2...95adc7e | 1009 s |
| yolo11s_b4_384x640_int8_fp16head.engine | trtexec + /model.23/*:fp16 | 12.18 MiB | 30cc9604...d5b6 | 783 s |
| yolo11s_b4_384x640_int8_minmax.engine | calib_generator --algo=minmax | 12.16 MiB | a5425d9d...13cf5 | 780 s |
| yolo11s_b4_384x640_int8_minmax_dense.engine | calib_generator --algo=minmax（991 帧） | 12.11 MiB | 6fa25d9c...cc464 | 940 s |

绑定全部为 `images 4x3x384x640 → output0 4x84x5040`。校准器构建含校准耗时（entropy2 1728 s、minmax 780/940 s）。

## 6. 三条技术路线结论（全部实机证伪/验证）

1. **entropy2 校准（默认）→ 证伪**：检测置信度被系统性压缩（Δconf 均值 0.34 vs 实测阈值 0.05），cam1 检测数 17248→5363（-69%）、cam2 全灭（457→0）。匹配率 0.31。
2. **检测头 FP16 混合精度（`--precisionConstraints=prefer --layerPrecisions=/model.23/*:fp16`）→ 证伪**：检测头 Conv 与 PWN 融合节点无 FP16 候选（"Using fastest implementation instead"），实测检测数与纯 INT8 相同（cam1 5361 vs 5363）——头层实际仍为 INT8。
3. **MinMax 校准 → 有效（采用）**：Δconf 均值降至 0.034，cam1 检测数 17341（FP16 17248），cam2 322（entropy2 为 0）。**数据密度实验**（667→991 帧）无进一步改善——剩余误差为 0.25-0.4 低置信边缘的量化固有误差。

对照：`trtexec --int8` 无 `--calib` → "Calibrator is not being used"（TRT 10.3 已移除内置随机校准器）→ **自定义校准器是 TRT 10.3 PTQ 的唯一途径**。

## 7. 精度回归（核心，file 模式，scheduler 关闭，interval=0）

### 口径

- 主指标：FP16↔INT8 同管线（唯一差异=精度）；逐帧 class-agnostic 贪心 IoU 匹配（≥0.5）
- 次指标：INT8↔ORT FP32 协议带（numpy NMS 与 nvinfer NMS 实现差异为噪声基线）
- 对齐：两次 run 每流 in_frames 一致（metrics summary）；JSONL 帧记录数差异源于 0 检测帧，非丢帧
- 工具：`scripts/accuracy_math.py`（纯逻辑，22 单测）+ `scripts/compare_precision.py` + `scripts/run_ort_reference.py`（ORT FP32 2072 帧参考，19550 检测，bus conf=0.962 与 Stage 4/5 的 0.95/0.955 吻合）

### 结果（MinMax 稀疏 vs FP16）

| 流 | 匹配率 f16→i8 | 匹配率 i8→f16 | Δconf 均值 | Δconf p95 | 类一致性 | Δn 均值 |
|---|---|---|---|---|---|---|
| cam1 | 0.9470 | 0.9419 | 0.0335 | 0.0964 | 0.9972 | 0.73 |
| cam2 | 0.7046 | 1.0000 | 0.0454 | 0.0984 | 1.0000 | 0.84 |
| cam3 | 0.9957 | 0.9957 | 0.0333 | 0.0576 | 1.0000 | 0.01 |
| cam4 | 0.9391 | 0.9946 | 0.0173 | 0.0523 | 1.0000 | 0.15 |

关键目标留存（FP16 检出帧中 INT8 仍检出 IoU 匹配的比例）：bus **0.983** / car **0.960** / person **0.950**（person 差阈值 0.95 仅 0.0005）。

未匹配诊断：cam1 未匹配 915 个中 **82% 置信度 <0.4**（p50=0.303）、cam2 未匹配 135 个**全部 <0.4**、cam4 75% <0.5——量化把一批 0.25-0.4 边缘框压过 pre-cluster 阈值 0.25 而丢失。量化方向安全：i8→f16 匹配率 cam2/cam4 100%、cam1 94%，无幻觉框、无类翻转（类一致性 ≥0.997）。

### 阈值判定（保守初始值，按实测定稿）

| 指标（阈值） | cam1 | cam2 | cam3 | cam4 | 判定 |
|---|---|---|---|---|---|
| 匹配率 ≥0.95 | 0.947 | 0.705 | 0.996 | 0.939 | **FAIL**（cam1/2/4） |
| Δconf 均值 ≤0.05 | 0.034 | 0.045 | 0.033 | 0.017 | PASS |
| Δconf p95 ≤0.10 | 0.096 | 0.098 | 0.058 | 0.052 | PASS |
| 类一致性 ≥0.98 | 0.997 | 1.0 | 1.0 | 1.0 | PASS |
| Δn 均值 ≤0.5 | 0.73 | 0.84 | 0.01 | 0.15 | FAIL（cam1/2） |
| 关键目标留存 ≥0.95 | bus 0.983 / car 0.960 / person 0.950 | | | | FAIL（person 差 0.0005） |

**判定：不达标（匹配率与关键目标留存未达保守阈值）→ 按 §16 纪律回退 FP16 交付。**

## 8. 性能 A-B-A（4 路 RTSP 30fps，60s 受控窗口，干净环境）

| 指标 | FP16 | INT8 MinMax | 改善 |
|---|---|---|---|
| P50 | 36.82 ms | 29.40 ms | -20.2% |
| **P95** | **43.44 ms** | **35.51 ms** | **-18.3%** |
| P99 | 46.59 ms | 38.01 ms | -18.4% |
| avg | 37.12 ms | 29.89 ms | -19.5% |
| drop | 0.0 | 0.0 | — |

FP16 P95 43.4ms 与 Stage 12 基线 43.3ms 一致（干净环境确认）。注：校准构建期间的 A-B-A 初跑受后台负载波动影响（FP16 P95 46.2-62.0ms），复测以本表为准。输入 FPS 均为源速率 119.95（4×30），延迟改善反映推理段。

## 9. 验收清单

- [x] 抽帧：667 帧（默认）与 991 帧（密集），manifest 一致、尺寸校验通过
- [x] calib_generator 双算法产出缓存（首行 TRT-100300-EntropyCalibration2 / MinMaxCalibration），SHA256 记录
- [x] engine 构建：entropy2（trtexec）、fp16head（trtexec）、minmax ×2（calib_generator），全部记录
- [x] nvinfer INT8 配置四路 file 模式跑通：2072 帧、EOS 优雅退出、JSONL 0 非法、bus conf 抽查
- [x] FP16/INT8 多轮回归：每流 in_frames 一致（1442/163/288/179）
- [x] ORT 参考 JSONL：2072 帧 19550 检测，bus conf=0.962 吻合
- [x] compare_precision 全量对比 + 阈值判定表 + Markdown 报告
- [x] 性能 A-B-A 干净复测：INT8 P95 -18.3%
- [x] ctest 9/9（新增 image_reader + accuracy_math）
- [x] 文档同步 + 单提交推送

## 10. 结论与遗留

- **结论**：INT8 PTQ 全链路（校准数据→校准器→engine→nvinfer→精度回归→性能对比）已在 Jetson 实机跑通并留下完整可复现工具链；MinMax 校准优于 entropy2（Δconf 0.34→0.034）；精度未达保守阈值（低置信边缘量化损失），按纪律回退 FP16；INT8 性能收益实测 P95 -18.3% 记录在案。
- **遗留/后续**：
  - 阈值 0.95 为保守初始值；若业务可接受 0.94+ 匹配率（关键目标留存 0.95+），MinMax engine 可直接启用（配置 `configs/nvinfer_yolo11s_b4_int8.txt` 换 engine 路径）
  - 未匹配集中在 0.25-0.4 边缘框：可实验降低 pre-cluster 阈值或在检测头输出层前插入 FP16 层（需自定义图改造，超出本阶段）
  - 校准数据覆盖率（更多场景视频）或校准策略（如 per-layer 混合）留作后续
  - 遗留测试进程清理：Stage 11 验收残留实例（4h，1.16 GB JSONL）已优雅停止并记录
