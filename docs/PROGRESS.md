# JetEdge-Agent Progress

最后更新时间：2026-08-01

## 当前结论

- 阶段 0（环境核查）：已完成 ✓
- 阶段 1（单路硬件解码）：已完成 ✓
- 阶段 3（YOLO11s ONNX 导出、验证、传输与 SHA256 一致性验收）：已完成 ✓
- 旧方案阶段 2（四路 streammux + fakesink）：已完成 ✓，代码将在新方案 Stage 5 中复用

当前准备进入阶段 4：TensorRT FP16 Engine 构建和单路 DeepStream `nvinfer` 验证。

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

## 阶段 4 范围

阶段 4 只做：

1. 在 Jetson 上检查 TensorRT、DeepStream、GStreamer 和 `nvinfer` 实际环境。
2. 在 Jetson 上基于已验收的 ONNX 构建 TensorRT FP16 Engine。
3. 记录构建命令、版本、binding、warning、Engine 大小和 SHA256。
4. 用单路本地视频跑通 DeepStream `nvinfer`。
5. 验证 YOLO11 输出解析、检测框、类别、置信度、EOS、Ctrl-C 和内存行为。

阶段 4 暂时不做：

- 四路视频；
- Tracker；
- Metrics 框架；
- RTSP；
- Kimi；
- DeepSeek；
- Agent；
- 阶段 5 到阶段 8 的大量代码。
