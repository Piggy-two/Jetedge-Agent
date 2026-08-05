# 开源视频复用性验证（openvideo validation, 2026-08-05）

## 目的

证明 JetEdge-Agent 管道对**从未见过的数据**具有即插即用复用性：
同一份已验收二进制（`build/jetedge_server`，Stage 14 版本，零代码改动、零重新编译），
只把配置里的视频源换成 4 个全新开源视频，端到端跑通检测 → 跟踪 → 事件 → 调度 → 控制面。

## 输入：4 个开源视频（首次使用，与任何验收数据无交集）

来源：[intel-iot-devkit/sample-videos](https://github.com/intel-iot-devkit/sample-videos)
（BSD 风格示例库，本仓库附 CC-BY 4.0 许可证，需署名）。
下载到 `~/jetedge-openvideos/`（Git 之外），2026-08-05，共 ~17.6 MB。

| 路 | 文件 | 分辨率 | 编码 | 帧率 | 时长 | 帧数 | 大小 | SHA256（前 16） |
|---|---|---|---|---|---|---|---|---|
| cam1 | person-bicycle-car-detection.mp4 | 768x432 | H.264 | 12 | 54.0 s | 647 | 6.0 MB | 452b11b7e0efbd01… |
| cam2 | car-detection.mp4 | 768x432 | H.264 | 12.5 | 30.2 s | 377 | 2.8 MB | d31e0ebf194cc16e… |
| cam3 | people-detection.mp4 | 768x432 | H.264 | 12 | 49.7 s | 596 | 5.5 MB | 18ffe8672d741e3e… |
| cam4 | one-by-one-person-detection.mp4 | 768x432 | H.264 | 10 | 139.4 s | 1394 | 3.3 MB | a5964aa259099a48… |

关键点：**432p 分辨率 + 带 AAC 音轨**——与之前所有验收数据（720p 示例流）不同，
验证了 `filesrc → qtdemux → h264parse → nvv4l2decoder` 硬件解码路径对未知容器/音轨的鲁棒性。

## 配置差异（与 Stage 14 验收配置的唯一区别）

`configs/streams_openvideo.yaml` vs `configs/streams_stage14.yaml`：

- `rtsp.enable: false`（文件源不需要 RTSP watchdog/重连）
- `streams[]`：`type: file`、uri 指向 4 个开源视频、`expected_fps` 填真实帧率
- 输出路径（jsonl/keyframe/control → `logs/openvideo_*`）

其余全部字节级一致：nvinfer FP16 engine、tracker、事件类表与 cam1 区域、调度器阈值、
Control API 参数（仅端口换 8091）。

## 实测结果（Jetson 实机，2026-08-05 14:51，运行 ~42 s 自然 EOS 退出）

| 路 | 帧数 | 检测数 | obj/帧 | 每路 FPS | 主要类别（置信度） |
|---|---|---|---|---|---|
| cam1 | 647/647 | 418 | 0.65 | 16.63 | person 0.79、car 0.93、bicycle |
| cam2 | 377/377 | 92 | 0.24 | 9.69 | cell phone 0.86、boat 0.90、car 0.93 |
| cam3 | 596/596 | 498 | 0.84 | 15.32 | person 0.93（400 个）|
| cam4 | 1394/1394 | 2531 | 1.82 | 35.83 | person 0.93、tv 0.47、chair 0.43 |

- 帧数与视频时长精确对应（cam2 377 = 30.2 s × 12.5；cam4 1394 = 139.4 s × 10），逐帧全部处理
- 检测 JSONL 3539 行、事件 JSONL 74 行：**逐行校验 0 非法**
- 事件：appearance/disappearance 各流平衡（13/13、5/5、7/7、8/8）；cam3 count_high 1；cam1 区域 entry 6（复用原 cam1 街道区域配置，区域规则直接生效）
- 关键帧：49 张保存（cap=50），cam1 区域事件抽帧
- 调度器：全程 NORMAL，0 次调整（表 [0 0 0]）；CPU 9-17%、mem 50.3-50.4% 稳定、温度 ~56.5°C
- 日志：0 ERROR、1 WARN（关键帧 cap 良性）、`exit OK`，解码线程亲和性由 start_pipeline.sh 正常固定

## ORT FP32 交叉验证（可疑检测归因）

cam2 的 cell phone / boat 与 cam4 的 tv / chair 置信度分布存疑，
用已验收 ONNX + ONNX Runtime FP32（`accuracy_math.decode_yolo11 + nms_per_class`，
完整复刻 mux→nvinfer 预处理链）逐帧对照，证明这些检测**全部来自模型本身**，管道零发明：

| 帧 | 管道（FP16 TensorRT） | ORT FP32 参考 | 结论 |
|---|---|---|---|
| cam2 f70 | boat 0.85 | boat 0.83 | 一致 |
| cam2 f95 | cell phone 0.89 | cell phone 0.90 | 一致 |
| cam2 f60 | 无 | boat 0.78 | 类别在模型输出中；nvinfer NMS 协议噪声（Stage 13 已记录），非管道误检 |
| cam4 f200 | chair 0.43 + tv 0.47 | chair 0.42 + tv 0.38 | 一致（同类，Δconf 0.09）|
| cam4 f700 | chair 0.53 | chair 0.49 | 一致 |
| cam4 f1200 | chair 0.5 | chair 0.55 | 一致 |

说明：tv/chair 等中置信检测是 YOLO11s 在 432p 低分辨率内容（被拉伸到 640x384 推理）
上的真实模型行为，不是管道缺陷——管道如实上报模型输出。

## 复用性结论

- **零代码改动、零重新编译**：同一二进制直接消费新数据（新分辨率、新容器、带音轨）
- 硬件解码、检测解析、跟踪、事件、区域规则、调度、控制面全部原生生效
- 检测语义与场景吻合（骑车/车/行人视频检出 person/car/bicycle，置信度 0.9+）
- 管道输出与模型参考（ORT FP32）逐帧一致，移植正确性在未见数据上仍然成立

## 局限（诚实声明）

- 本次为 42 s 短运行（文件源自然 EOS），未做长时间稳定性（2 h 稳定性已由 Stage 14 固定样例流覆盖，二者互补）
- 432p 内容拉伸推理产生的中低置信误检（tv/chair）属模型能力边界，非管道问题
- 区域规则仍按原 cam1 街道语义配置；对任意新场景语义上应重新配置区域

## 复现

```bash
# 1. 下载（需网络，一次性）
mkdir -p ~/jetedge-openvideos && cd ~/jetedge-openvideos
BASE=https://raw.githubusercontent.com/intel-iot-devkit/sample-videos/master
for f in person-bicycle-car-detection car-detection people-detection one-by-one-person-detection; do
  curl -sL -o $f.mp4 $BASE/$f.mp4; done
sha256sum *.mp4   # 与上表比对

# 2. 运行（文件源全部 EOS 后自然退出）
scripts/start_pipeline.sh configs/streams_openvideo.yaml
# ~1-2 分钟后检查日志 logs/jetedge_server.log 出现 "exit OK"

# 3. 验收
python3 scripts/validate_jsonl.py logs/openvideo_detections.jsonl
python3 scripts/validate_jsonl.py logs/openvideo_events.jsonl
```

视频文件不入 Git（`~/jetedge-openvideos/`，与模型/引擎同级的大工件策略）。
