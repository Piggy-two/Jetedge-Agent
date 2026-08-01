# JetEdge-Agent Progress

最后更新时间：2026-08-01

## 当前结论

- 阶段 0（环境核查）：已完成 ✓
- 阶段 1（单路硬件解码）：已完成 ✓
- 阶段 3（YOLO11s ONNX 导出、验证、传输与 SHA256 一致性验收）：已完成 ✓
- 旧方案阶段 2（四路 streammux + fakesink）：已完成 ✓，代码将在新方案 Stage 5 中复用
- **阶段 4（TensorRT FP16 Engine + 单路 nvinfer 验证）：已完成 ✓（2026-08-01）**

当前准备进入阶段 5：四路检测、Tracker、结构化输出和 Metrics。

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

阶段 5 待做：四路检测（nvinfer batch-size=4）、Tracker、结构化输出和 Metrics。
