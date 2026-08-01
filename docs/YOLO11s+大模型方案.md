# YOLO11s + 大模型方案

> 项目名称：JetEdge-Agent
> 项目定位：基于 Jetson Orin Nano 8GB 的多路视频实时感知、多模态事件理解与智能运维平台
> 核心技术：C++、Linux、DeepStream、GStreamer、TensorRT、YOLO11s、Tracker、事件引擎、性能监控、DeepSeek API、Kimi API、Agent 工具调用
> 文档版本：V1.1（低消耗与低延迟优化版）
> 日期：2026-07-31

---

## 1. 方案概述

JetEdge-Agent 不定位为“在 Jetson 上运行一个 YOLO 模型”，而是建设一套完整的边缘智能系统：

1. Jetson 使用 DeepStream 和 TensorRT 对四路视频进行实时解码、批处理、检测与追踪；
2. YOLO11s 负责高频、低延迟的结构化视觉感知；
3. Tracker 负责补充目标时序关系，减少 YOLO 的逐帧推理压力；
4. 本地事件规则负责发现 ROI 入侵、越线、停留、断流、帧率下降和温度异常；
5. 本地路由器先判断事件是否真的需要大模型，能够由规则确定的事件不调用模型；
6. Kimi 多模态模型只处理少量有歧义的关键帧或高价值视觉事件；
7. DeepSeek V4 Flash 只处理聚合后的日志、指标和候选运维计划；
8. 本地 Agent 对模型建议进行权限校验，只执行受控白名单工具；
9. 所有操作都必须具备审计、验证和必要的回滚能力。

整体原则：

```text
YOLO11s：负责实时检测
Tracker：负责目标时序关系和检测间隔补帧
事件规则：负责低成本事件发现
事件路由：判断是否需要调用大模型
Kimi：负责少量关键帧的视觉语义确认
DeepSeek：负责聚合指标诊断和候选策略生成
本地 Agent：负责安全、受控地执行
```

本方案的核心不是让大模型进入实时视频主链路，而是采用：

```text
边缘实时感知 + 本地规则决策 + 云端按需理解 + 本地受控执行
```

这样可以同时降低：

- Jetson 计算资源占用；
- 大模型 API 调用次数；
- 图片上传量；
- Token 消耗；
- 云端调用费用；
- 大模型等待时间对实时链路的影响。

---

## 2. 项目目标

### 2.1 核心目标

实现一套能够稳定处理四路视频的 Jetson 边缘 AI 平台，具备：

- 多路视频硬件解码；
- TensorRT FP16 实时检测；
- 多目标追踪；
- 检测间隔与 Tracker 协同；
- ROI、越线和停留等事件分析；
- FPS、延迟、GPU、内存、温度和功耗监控；
- 事件去重、合并、冷却和分级；
- 关键帧智能筛选和压缩；
- 云端多模态事件理解；
- 云端文本诊断与策略生成；
- 大模型请求分级路由；
- 本地 Agent 工具调用；
- 操作验证、审计和回滚；
- GitHub 阶段化开发管理；
- Benchmark、报告和 Demo 输出。

### 2.2 优化目标

在满足功能的基础上，进一步实现：

```text
规则能够处理的事件不调用大模型
普通视觉事件只调用 Kimi
系统性能异常只调用 DeepSeek
高风险复杂事件才串联 Kimi 和 DeepSeek
默认只发送 1 张关键帧
普通请求默认使用非思考模式
大模型请求不阻塞视频主线程
本地告警先返回，大模型结果后补充
```

### 2.3 简历价值

项目最终应体现以下能力，而不是只突出模型调用：

```text
C++ / Linux 工程开发
DeepStream / GStreamer 多路 Pipeline
TensorRT 模型部署与性能优化
GPU 异构计算和边缘端资源管理
Tracker 与视频事件系统
异步队列和线程安全
事件分级与请求路由
HTTP Keep-Alive 与连接池
结构化输出与 Schema 校验
Agent Loop、工具调用和安全策略
系统故障恢复与可观测性
大模型成本与延迟优化
```

### 2.4 非目标

当前版本不追求：

- 在 Jetson 上训练 YOLO；
- 在 Jetson 上常驻运行 7B/8B 大模型；
- 把每一帧都发送给多模态 API；
- 每个事件固定调用 Kimi 和 DeepSeek；
- 第一版就实现复杂 ReID；
- 第一版就接入大型 Agent 框架；
- 让大模型直接执行任意 Shell；
- 把视频、ONNX、Engine 或 API Key 上传到 GitHub。

---

## 3. 模型选型

## 3.1 实时检测模型：YOLO11s

默认选择：

```text
模型：YOLO11s
来源：Ultralytics COCO pretrained
输入尺寸：1×3×384×640
推理精度：TensorRT FP16
Batch：第一版固定为 1
类别：COCO 80 类
```

选择 YOLO11s 的原因：

- 相比 nano 模型具有更好的检测能力；
- 模型规模仍适合 Orin Nano；
- 能作为四路检测的正式主模型；
- 后续可以增加 YOLO11n 作为性能模式；
- 可以开展 FP16、INT8 和 infer interval 对比实验；
- 适合展示部署、调度和性能权衡能力。

建议最终保留三种运行配置：

| 配置 | 模型 | infer interval | 主要用途 |
|---|---|---:|---|
| performance | YOLO11n | 2 | 四路压力测试、低功耗和降级模式 |
| balanced | YOLO11s | 1 | 项目默认运行模式 |
| quality | YOLO11s | 0 | 高检测频率和高质量模式 |

第一阶段只完成 YOLO11s FP16，其他模式在主链路稳定后增加。

### YOLO 侧降低计算量的方法

1. 使用 `infer interval + Tracker`，不必每帧运行检测；
2. 场景空闲时降低检测频率，事件出现后临时提高频率；
3. GPU 温度过高或队列积压时自动进入性能模式；
4. Benchmark 时关闭非必要 OSD 和显示；
5. 后续增加 INT8 Engine 作为性能实验；
6. 保留 YOLO11n 作为资源紧张时的降级模型。

---

## 3.2 文本模型：DeepSeek V4 Flash

默认用途：

- 分析 FPS 和延迟异常；
- 分析 GPU、内存、温度和功耗；
- 总结最近错误日志；
- 判断可能故障原因；
- 生成运维建议；
- 为本地 Agent 生成候选工具调用计划；
- 输出结构化事件或运行日报。

默认配置：

```text
Provider：DeepSeek
Base URL：https://api.deepseek.com
Model：deepseek-v4-flash
API Key 环境变量：DEEPSEEK_API_KEY
```

DeepSeek 不接收高频逐帧数据，也不接收大量原始日志，只接收经过本地聚合后的结构化信息：

```json
{
  "event_type": "stream_fps_drop",
  "stream_id": 2,
  "expected_fps": 30.0,
  "current_fps": 8.4,
  "p95_latency_ms": 142.6,
  "queue_depth": 27,
  "gpu_usage_percent": 96,
  "memory_used_mb": 6820,
  "temperature_c": 78,
  "error_counts": {
    "decoder_timeout": 12,
    "queue_overflow": 4
  },
  "new_errors": [
    "decoder timeout"
  ]
}
```

期望模型输出严格 JSON：

```json
{
  "severity": "warning",
  "summary": "Stream 2 FPS dropped significantly.",
  "likely_causes": [
    {
      "cause": "gpu_or_queue_overload",
      "confidence": 0.78
    }
  ],
  "recommended_actions": [
    {
      "tool": "set_infer_interval",
      "arguments": {
        "stream_id": 2,
        "interval": 2
      },
      "risk": "low",
      "reason": "Reduce inference load."
    }
  ],
  "need_human_confirmation": false
}
```

### DeepSeek 降低 Token 和延迟的方法

- 普通诊断默认使用非思考模式；
- 只有复杂、反复出现且本地规则无法解释的故障才升级思考模式；
- 固定 System Prompt、Schema 和工具列表放在请求前部；
- 动态事件数据放在请求尾部，便于上下文缓存；
- 不上传原始长日志，只上传 Top-N 错误和聚合统计；
- 普通事件诊断输出限制在约 384 Token；
- Agent 候选计划输出限制在约 256 Token；
- 日报类请求批量聚合，不逐事件生成长文本。

模型输出只能作为候选计划，不能直接等同于操作命令。

---

## 3.3 多模态模型：Kimi K2.6

默认用途：

- 描述事件关键帧；
- 判断关键帧中的场景关系；
- 对 ROI、占道、拥堵、停留等事件进行二次确认；
- 判断规则事件是否可能为误报；
- 生成面向用户的自然语言告警。

默认配置：

```text
Provider：Moonshot Kimi
Base URL：https://api.moonshot.cn/v1
Model：kimi-k2.6
API Key 环境变量：MOONSHOT_API_KEY
```

第一版优先发送事件关键帧，不发送完整长视频。

### 图片调用方式

```text
本地关键帧
   ↓
目标 ROI 裁剪或全景缩放
   ↓
JPEG 压缩
   ↓
Base64 Data URL
   ↓
Kimi API
```

### Kimi 降低图片和 Token 消耗的方法

```text
默认：1 张关键帧
存在遮挡或上下文不足：2 张
必须判断前后变化：3 张
```

普通事件优先发送：

```text
目标 bbox 周围扩展 20%～40% 的 ROI 图片
```

只有判断道路、车道、拥堵和目标关系时，才发送缩放后的全景图。

建议图片参数：

```yaml
vision_input:
  default_images_per_event: 1
  max_images_per_event: 3
  max_long_edge: 768
  jpeg_quality: 75
  roi_padding_ratio: 0.3
```

关键帧之间应使用以下方法去重：

- 感知哈希；
- 目标 bbox 位移；
- 目标数量变化；
- 类别变化；
- 帧间时间距离。

普通视觉确认默认使用非思考模式；高风险、复杂场景或多目标关系判断再升级思考模式。

期望返回：

```json
{
  "event_confirmed": true,
  "scene_description": "A pedestrian remains inside a vehicle lane.",
  "risk_level": "medium",
  "observations": [
    "One pedestrian is visible.",
    "Several vehicles are passing nearby."
  ],
  "possible_false_positive": false,
  "recommended_summary": "Pedestrian detected in the vehicle lane."
}
```

Kimi 只负责理解视觉事件，不直接生成或执行系统操作。

---

## 4. 低消耗、低延迟总体架构

原始串行调用方式：

```text
事件 → Kimi → DeepSeek → Agent
```

容易产生以下问题：

- 每个事件调用两个模型；
- Kimi 和 DeepSeek 串行等待；
- 图片、Token 和调用费用增加；
- 普通事件也承受复杂模型延迟；
- 大模型故障可能拖慢事件处理。

优化后的调用方式：

```text
YOLO + Tracker + 本地规则
          ↓
事件去重、合并、置信度评分
          ↓
事件路由器
          ↓
├─ 本地规则能够确定       → 本地记录和告警，不调用模型
├─ 视觉语义存在歧义       → 只调用 Kimi
├─ 系统性能或日志异常     → 只调用 DeepSeek
└─ 高风险且需要自动操作   → Kimi/DeepSeek 按依赖关系调用
```

### 4.1 事件分级

| 级别 | 示例 | 默认处理 |
|---|---|---|
| L0 | 短时 FPS 波动、普通目标出现 | 本地记录，不调用模型 |
| L1 | 明确的 ROI 入侵、越线 | 本地规则确认，可直接告警 |
| L2 | 视觉语义不确定、疑似误报 | Kimi 二次确认 |
| L3 | 断流、持续掉帧、温度异常 | DeepSeek 诊断 |
| L4 | 高风险视觉事件且需要系统调整 | 视觉确认 + 文本规划 + Agent |

### 4.2 用户感知延迟优化

本地告警不能等待大模型：

```text
事件发生
   ↓
本地规则立即生成告警
   ↓
界面显示：AI 分析中
   ↓
后台调用 Kimi 或 DeepSeek
   ↓
收到结果后补充事件解释
```

示例：

```text
23:10:01  检测到疑似行人进入 ROI
23:10:01  状态：AI 确认中
23:10:04  Kimi 确认：行人位于机动车道，风险中等
```

### 4.3 并行而非固定串行

如果视觉分析和系统指标分析互不依赖：

```text
             ┌→ Kimi 视觉分析
事件快照 ────┤
             └→ DeepSeek 指标分析
```

只有当 DeepSeek 的输入必须依赖 Kimi 结论时才串行执行。

---

## 5. 系统总体架构

```text
┌───────────────────────────────────────────────────────┐
│                   四路视频输入                         │
│ 本地 MP4 / H.264 / 后续 RTSP                          │
└───────────────────────┬───────────────────────────────┘
                        ↓
┌───────────────────────────────────────────────────────┐
│ DeepStream / GStreamer                                │
│ Source Bin → nvv4l2decoder → Queue → nvstreammux      │
└───────────────────────┬───────────────────────────────┘
                        ↓
┌───────────────────────────────────────────────────────┐
│ YOLO11s TensorRT FP16 + nvinfer                       │
│ 配置化 infer interval，输出 bbox/class/confidence     │
└───────────────────────┬───────────────────────────────┘
                        ↓
┌───────────────────────────────────────────────────────┐
│ Tracker + Pad Probe                                   │
│ track_id / 轨迹 / 生命周期 / 检测间隔补帧             │
└───────────────────────┬───────────────────────────────┘
                        ↓
┌───────────────────────────────────────────────────────┐
│ Event Engine                                          │
│ ROI / 越线 / 停留 / FPS下降 / 断流 / 温度异常         │
└───────────────────────┬───────────────────────────────┘
                        ↓
┌───────────────────────────────────────────────────────┐
│ Event Filter + Router                                 │
│ 去重 / 冷却 / 合并 / 分级 / 置信度 / 调用路由         │
└──────────────┬───────────────────────┬────────────────┘
               ↓                       ↓
┌───────────────────────────┐  ┌────────────────────────┐
│ Keyframe Worker           │  │ Metrics Aggregator     │
│ ROI裁剪/压缩/相似帧过滤    │  │ 窗口聚合/状态变化检测   │
└──────────────┬────────────┘  └───────────┬────────────┘
               ↓                           ↓
┌───────────────────────────┐  ┌────────────────────────┐
│ Kimi API                  │  │ DeepSeek API           │
│ 低频视觉语义确认           │  │ 低频诊断和候选计划      │
└──────────────┬────────────┘  └───────────┬────────────┘
               └──────────────┬────────────┘
                              ↓
┌───────────────────────────────────────────────────────┐
│ Unified Analysis Result                               │
│ 统一事件结论、风险、建议和候选操作                    │
└───────────────────────┬───────────────────────────────┘
                        ↓
┌───────────────────────────────────────────────────────┐
│ Agent Policy + Tool Executor                          │
│ 白名单、Schema、参数范围、确认、执行、验证、回滚      │
└───────────────────────┬───────────────────────────────┘
                        ↓
┌───────────────────────────────────────────────────────┐
│ Audit / Event Store / Benchmark Report                │
└───────────────────────────────────────────────────────┘
```

---

## 6. 本机与 Jetson 职责划分

## 6.1 本地开发主机负责

- 管理项目方案、文档和 GitHub；
- 创建 Python 隔离环境；
- 下载 YOLO11s PyTorch 权重；
- 导出 YOLO11s ONNX；
- 使用 ONNX Checker 和 ONNX Runtime 验证；
- 生成 `model_info.txt`；
- 准备和规范化测试视频；
- 使用 VS Code Remote-SSH 连接 Jetson；
- 将 ONNX 和模型信息复制到 Jetson；
- 保存 Benchmark、截图和 Demo 素材。

## 6.2 Jetson 负责

- TensorRT Engine 构建；
- DeepStream / GStreamer Pipeline；
- `nvv4l2decoder` 硬件解码；
- `nvstreammux` 多路 Batch；
- `nvinfer` 推理；
- YOLO11 输出解析；
- Tracker；
- Metrics；
- Event Engine；
- Event Filter 和 Router；
- Keyframe Worker；
- DeepSeek 与 Kimi API 客户端；
- HTTP 连接池和 Worker Pool；
- Agent 工具执行；
- 稳定性、压力和故障恢复测试；
- ftrace、CPU Affinity 和性能优化。

## 6.3 核心约束

```text
本机不构建 Jetson TensorRT Engine
Jetson 不重新导出 PyTorch 模型
视频和模型大文件不进入 Git
API 请求不阻塞视频主线程
规则能够确定的事件不调用模型
大模型不能直接执行 Shell
```

---

## 7. 阶段实施路线

```text
阶段 3：YOLO11s ONNX 导出
阶段 4：TensorRT + nvinfer
阶段 5：四路检测、Tracker、Metrics
阶段 6：事件系统、去重路由和关键帧抽取
阶段 7：DeepSeek + Kimi API 与低延迟优化
阶段 8：Agent 工具调用和自动运维
```

每个阶段必须完成真实验收后才能进入下一阶段。

---

## 8. 阶段 3：YOLO11s ONNX 导出

### 8.1 目标

```text
YOLO11s PyTorch
   ↓
固定输入 ONNX
   ↓
结构检查
   ↓
ONNX Runtime 推理
   ↓
模型信息记录
   ↓
复制到 Jetson
```

### 8.2 导出参数

```python
from ultralytics import YOLO

model = YOLO("yolo11s.pt")

model.export(
    format="onnx",
    imgsz=(384, 640),
    batch=1,
    opset=12,
    simplify=True,
    dynamic=False
)
```

最终文件：

```text
models/yolo11s.onnx
models/model_info.txt
```

### 8.3 必须记录的信息

- 模型名和来源；
- Ultralytics、PyTorch、ONNX 和 ONNX Runtime 版本；
- 输入节点名称、shape 和 dtype；
- 所有输出节点名称、shape 和 dtype；
- COCO 80 类完整名称；
- ONNX opset；
- batch；
- 输入尺寸；
- dynamic 和 simplify；
- 模型文件大小；
- SHA256；
- 导出命令；
- 验证结果。

### 8.4 验收

```text
[ ] ONNX 文件存在
[ ] 输入 shape 为 1×3×384×640
[ ] onnx.checker 检查通过
[ ] ONNX Runtime 推理成功
[ ] 输出不含 NaN / Inf
[ ] model_info.txt 完整
[ ] ONNX 未进入 Git
[ ] 已复制到 Jetson
[ ] 本机和 Jetson SHA256 一致
```

---

## 9. 阶段 4：TensorRT 与 nvinfer

### 9.1 Engine 构建

```text
模型：YOLO11s
输入：1×3×384×640
精度：FP16
Batch：1
```

输出：

```text
models/yolo11s_b1_384x640_fp16.engine
```

记录：

- TensorRT 版本；
- GPU 与 JetPack 信息；
- 构建命令；
- Binding；
- 构建耗时；
- Engine 大小；
- Workspace；
- SHA256；
- 警告和错误。

### 9.2 nvinfer 接入

至少验证：

- 输出 Tensor 读取；
- bbox 解码；
- 类别和置信度；
- NMS；
- letterbox 坐标还原；
- 边界裁剪；
- COCO 类别映射；
- 检测框位置合理。

### 9.3 单路验收

```text
[ ] Engine 构建成功
[ ] nvinfer 加载成功
[ ] 单路硬件解码正常
[ ] 能输出检测框、类别和置信度
[ ] Pipeline 正常 EOS
[ ] Ctrl-C 能安全退出
[ ] 没有明显内存持续增长
```

---

## 10. 阶段 5：四路检测、Tracker 与 Metrics

### 10.1 四路 Pipeline

```text
4×Source
4×Decode
4×Queue
     ↓
nvstreammux
     ↓
nvinfer
     ↓
nvtracker
     ↓
Pad Probe / Metrics / Event
```

视频 URI、batch、阈值、推理间隔和 Tracker 参数必须配置化。

### 10.2 动态检测频率

建议设计三种状态：

```text
IDLE：无活动目标，降低检测频率
ACTIVE：检测到普通目标，使用默认频率
ALERT：目标接近 ROI 或触发事件，提高检测频率
```

系统负载过高时允许进入降级状态：

```text
GPU温度高 / 队列积压 / 内存不足
             ↓
提高 infer interval
             ↓
必要时切换 YOLO11n
```

### 10.3 Tracker 信息

至少输出：

- `stream_id`；
- `track_id`；
- class；
- bbox；
- confidence；
- 目标年龄；
- 目标丢失和恢复；
- 每路活动目标数量。

### 10.4 Metrics

至少采集：

```text
每路输入 FPS
每路输出 FPS
每路检测数量
每路 Tracker 数量
总吞吐量
平均延迟
P50 / P95 / P99 延迟
丢帧数
队列积压
CPU 使用率
系统内存
GPU 使用率
温度
功耗
运行时长
重连次数
```

### 10.5 验收

至少连续运行 30 分钟：

```text
[ ] 四路均能解码
[ ] Batch 正确
[ ] 四路均有检测结果
[ ] Tracker ID 可用
[ ] infer interval 生效
[ ] Metrics 持续输出
[ ] Pipeline 无崩溃
[ ] 内存没有明显持续增长
[ ] 正常退出
```

---

## 11. 阶段 6：事件系统、路由与关键帧抽取

### 11.1 第一版事件

视觉事件：

```text
person_in_roi
vehicle_in_roi
line_crossing
object_loitering
```

系统事件：

```text
stream_fps_drop
stream_disconnected
gpu_temperature_high
queue_backlog
```

### 11.2 事件数据结构

```json
{
  "event_id": "uuid",
  "event_type": "person_in_roi",
  "stream_id": 2,
  "track_id": 17,
  "timestamp": "2026-07-31T22:00:00+08:00",
  "severity": "warning",
  "confidence_score": 0.87,
  "llm_route": "kimi",
  "objects": [
    {
      "class_id": 0,
      "class_name": "person",
      "confidence": 0.87,
      "bbox": [100, 80, 220, 360]
    }
  ],
  "rule": {
    "name": "pedestrian_roi",
    "duration_ms": 5200
  },
  "metrics_snapshot": {},
  "keyframes": []
}
```

### 11.3 事件防抖与聚合

必须具有：

- 目标级去重；
- 冷却时间；
- 相同事件合并；
- 最大触发频率；
- 事件过期；
- 有界队列；
- 队列满时丢弃或降级策略；
- 同一目标连续事件合并；
- 同一时间窗口内系统异常聚合。

建议：

```text
视觉事件去重窗口：10 秒
系统指标聚合窗口：30 秒
同一目标只保留最新高质量关键帧
状态未变化时不重复调用 DeepSeek
```

### 11.4 关键帧策略

```text
默认关键帧：1 张
存在遮挡或上下文不足：2 张
必须判断变化过程：3 张
```

处理：

- ROI 裁剪；
- 长边限制；
- JPEG 压缩；
- 近似重复帧过滤；
- 保存原图和标注图；
- 关联 event_id；
- 异步写盘；
- 不阻塞 DeepStream 主线程。

### 11.5 验收

```text
[ ] 相同目标事件不会高频重复触发
[ ] 明确事件能够在本地完成处理
[ ] Kimi 与 DeepSeek 路由正确
[ ] 默认只生成 1 张关键帧
[ ] 重复关键帧能够过滤
[ ] 队列满时 Pipeline 不阻塞
```

---

## 12. 阶段 7：大模型 API 接入与加速

### 12.1 统一客户端抽象

```cpp
class ILlmClient {
public:
    virtual ~ILlmClient() = default;
    virtual AnalysisResult Analyze(
        const AnalysisRequest& request
    ) = 0;
};
```

实现：

```text
DeepSeekTextClient
KimiVisionClient
MockTextClient
MockVisionClient
```

### 12.2 异步结构

```text
DeepStream 主线程
     ↓
Event Queue
     ↓
Event Router
     ↓
Priority LLM Queue
     ↓
LLM Worker Pool
     ↓
HTTP API
     ↓
Schema Validator
     ↓
Event Store / Agent Planner
```

必须提供：

- 连接超时；
- 总请求超时；
- 有限重试；
- 指数退避；
- 并发限制；
- 队列长度限制；
- 优先级队列；
- 熔断；
- Mock 模式；
- API 开关；
- 请求 ID；
- 响应耗时；
- 首 Token 延迟；
- Token 用量；
- 图片大小；
- 日志脱敏。

### 12.3 HTTP 加速策略

C++ 客户端应：

```text
每个 Provider 维护长期客户端
启用 HTTP Keep-Alive
复用 TCP/TLS 连接
使用连接池
缓存 DNS 结果
区分连接超时和读取超时
启动时初始化客户端和线程池
```

不要每个事件重新创建 HTTP 客户端。

### 12.4 流式返回策略

流式输出只用于：

- Web 界面显示自然语言说明；
- 运维报告逐步展示；
- 缩短用户感知的首字等待时间。

Agent 工具调用必须等待完整 JSON 返回并完成 Schema 校验，不能根据半截流式内容执行操作。

### 12.5 优先级队列

```text
P0：高风险视觉事件
P1：断流、温度过高、持续掉帧
P2：普通 ROI 和越线事件
P3：日报和低优先级摘要
```

队列积压时：

```text
优先处理 P0 / P1
合并 P2
延迟或丢弃 P3
```

### 12.6 配置模板

```yaml
llm:
  routing:
    visual_event_use_deepseek: false
    system_event_use_kimi: false
    require_llm_for_rule_confirmed_event: false
    dedup_window_ms: 10000
    aggregation_window_ms: 30000
    max_events_per_batch: 10

  text:
    enabled: false
    provider: deepseek
    base_url: ${DEEPSEEK_BASE_URL}
    model: ${DEEPSEEK_MODEL}
    api_key_env: DEEPSEEK_API_KEY
    default_mode: non_thinking
    timeout_ms: 15000
    max_retries: 2
    max_concurrency: 2
    queue_capacity: 16
    max_tokens: 384
    stream_for_ui: true
    keep_alive: true

  vision:
    enabled: false
    provider: kimi
    base_url: ${KIMI_BASE_URL}
    model: ${KIMI_MODEL}
    api_key_env: MOONSHOT_API_KEY
    default_mode: non_thinking
    timeout_ms: 30000
    max_retries: 1
    max_concurrency: 1
    queue_capacity: 8
    default_images_per_event: 1
    max_images_per_event: 3
    max_long_edge: 768
    jpeg_quality: 75
    max_tokens: 256
    keep_alive: true

  escalation:
    enable_thinking_on_high_risk: true
    enable_deepseek_after_visual_confirmation: true
    require_human_confirmation_for_model_switch: true
```

环境变量模板：

```bash
DEEPSEEK_BASE_URL=https://api.deepseek.com
DEEPSEEK_MODEL=deepseek-v4-flash
DEEPSEEK_API_KEY=

KIMI_BASE_URL=https://api.moonshot.cn/v1
KIMI_MODEL=kimi-k2.6
MOONSHOT_API_KEY=
```

### 12.7 降级策略

当 API 不可用时：

```text
YOLO 继续运行
Tracker 继续运行
Metrics 继续采集
本地事件继续保存
本地告警继续输出
关键帧继续落盘
AI 分析状态标记为 unavailable
不得让 Pipeline 退出
```

### 12.8 阶段验收

```text
[ ] 大模型请求不阻塞视频主线程
[ ] 规则确定事件不调用模型
[ ] 视觉事件只调用 Kimi
[ ] 系统异常只调用 DeepSeek
[ ] 普通请求使用非思考模式
[ ] HTTP 连接能够复用
[ ] 首 Token 延迟和总延迟可记录
[ ] API 失败不影响 Pipeline
[ ] Mock 和真实 API 均能通过
```

---

## 13. 阶段 8：Agent 工具调用与自动运维

### 13.1 Agent Loop

```text
Observe
   ↓
Local Rule Check
   ↓
Analyze
   ↓
Plan
   ↓
Policy Check
   ↓
Execute
   ↓
Verify
   ↓
Success / Rollback / Alert
```

其中：

- 本地规则先判断是否能够直接处理；
- DeepSeek 负责复杂分析和候选 Plan；
- 本地策略模块负责 Policy Check；
- 本地 Tool Executor 负责 Execute；
- Metrics 和状态读取模块负责 Verify；
- 失败时执行回滚或告警。

### 13.2 白名单工具

只读工具：

```text
get_pipeline_status
get_stream_status
get_recent_metrics
get_recent_errors
get_gpu_status
get_event_summary
```

低风险修改：

```text
set_infer_interval
set_confidence_threshold
enable_stream
disable_stream
restart_stream
switch_model_profile
```

### 13.3 本地规则优先

以下场景不必调用 DeepSeek：

```text
GPU 温度超过明确阈值 → 进入性能模式
队列长度超过明确阈值 → 提高 infer interval
单路断流 → 按固定策略重连一次
恢复成功 → 记录审计日志
```

只有以下情况再升级给模型：

```text
连续重连失败
多个异常同时出现
本地规则执行后未恢复
异常原因不明确
存在多个候选操作
```

### 13.4 禁止能力

禁止大模型调用：

```text
任意 Shell
任意 sudo
任意文件删除
任意进程终止
任意网络扫描
任意 Git 操作
修改 SSH 配置
读取或回传完整 API Key
上传任意本地文件
```

### 13.5 参数边界

```text
infer interval：0～8
confidence threshold：0.1～0.9
同一路流 5 分钟最多重启 2 次
同一时间最多执行一个修改操作
模型切换默认需要人工确认
```

### 13.6 操作验证

例如执行：

```text
restart_stream(stream_id=2)
```

必须验证：

- 执行前状态；
- 工具返回结果；
- 流是否恢复；
- FPS 是否恢复；
- 错误是否减少；
- 验证是否超时；
- 是否需要回滚或升级告警。

不能仅根据函数返回值判断成功。

---

## 14. 推荐项目目录

```text
JetEdge-Agent/
├── CMakeLists.txt
├── README.md
├── CLAUDE.md
├── .env.example
├── config/
│   ├── pipeline.yaml
│   ├── pipeline.example.yaml
│   ├── nvinfer_yolo11s_fp16.txt
│   ├── tracker.yaml
│   ├── events.yaml
│   ├── llm.example.yaml
│   └── agent_policy.yaml
├── include/jetedge/
│   ├── pipeline/
│   ├── inference/
│   ├── tracking/
│   ├── metrics/
│   ├── events/
│   ├── routing/
│   ├── llm/
│   ├── agent/
│   └── control/
├── src/
│   ├── pipeline/
│   ├── inference/
│   ├── tracking/
│   ├── metrics/
│   ├── events/
│   ├── routing/
│   ├── llm/
│   ├── agent/
│   └── control/
├── scripts/
│   ├── export_yolo11s_onnx.py
│   ├── verify_onnx.py
│   ├── build_engine.sh
│   └── run_benchmark.sh
├── models/
│   └── model_info.txt
├── events/
│   ├── metadata/
│   ├── images/
│   └── clips/
├── benchmark/
│   ├── raw/
│   ├── reports/
│   ├── charts/
│   └── traces/
├── tests/
│   ├── inference/
│   ├── events/
│   ├── routing/
│   ├── llm/
│   └── agent/
└── docs/
    ├── development_log.md
    ├── model_export_report.md
    ├── tensorrt_engine_report.md
    ├── stage5_report.md
    ├── stage6_event_report.md
    ├── stage7_llm_report.md
    └── stage8_agent_report.md
```

应尊重现有工程结构，避免为了套用目录而大规模重构已工作的代码。

---

## 15. GitHub 管理方案

GitHub 用于同步：

- 源代码；
- CMake 和配置模板；
- 导出及验证脚本；
- `model_info.txt`；
- 环境检查报告；
- Benchmark 汇总；
- 项目开发日志；
- README；
- Agent 策略；
- API Mock 测试。

不进入 GitHub：

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
原始大日志
完整 Trace
事件关键帧
Demo 大视频
```

建议阶段提交：

```text
stage3-model-export
stage4-tensorrt-inference
stage5-multistream-metrics
stage6-event-routing
stage7-llm-integration
stage8-agent-operations
```

推荐提交信息：

```text
feat: add yolo11s onnx export workflow
feat: integrate yolo11s tensorrt inference
feat: add multi-stream tracking and metrics
feat: add event routing and keyframe filtering
feat: integrate low-latency deepseek and kimi clients
feat: add guarded agent tool execution
```

---

## 16. 安全与隐私

### 16.1 API Key

API Key 必须：

- 仅从环境变量读取；
- 不写入 C++ 常量；
- 不写入 YAML 正式配置；
- 不输出到日志；
- 不出现在异常栈；
- 不上传 GitHub；
- 不传给大模型自身；
- 支持后续轮换。

### 16.2 图片和业务数据

调用多模态 API 前应：

- 只发送事件相关关键帧；
- 优先发送 ROI 裁剪图；
- 进行尺寸压缩；
- 必要时进行人脸、车牌或敏感区域处理；
- 不发送无关的连续视频；
- 记录发送数据类型，不记录完整 Base64；
- 为事件图片设置本地清理周期。

### 16.3 模型输出

所有模型输出均视为不可信输入，必须：

- JSON 解析；
- Schema 校验；
- 字段长度限制；
- 枚举校验；
- 工具白名单校验；
- 参数范围校验；
- 风险分级；
- 必要时人工确认。

---

## 17. Benchmark 设计

### 17.1 YOLO11s 性能

测试：

```text
单路 1080p30
四路 1080p30
四路 720p30
不同 infer interval
OSD 开启/关闭
Tracker 开启/关闭
FP16 Engine
后续 INT8 Engine
YOLO11s 与 YOLO11n 降级模式
```

指标：

- 每路 FPS；
- 总 FPS；
- P50/P95/P99 延迟；
- GPU 使用率；
- 内存；
- 温度；
- 功耗；
- 丢帧；
- 稳定运行时间。

### 17.2 大模型 API

测试：

- DeepSeek 首 Token 延迟；
- DeepSeek 总请求延迟；
- Kimi 图片分析延迟；
- HTTP 连接复用前后延迟；
- 非思考与思考模式延迟；
- 1 张、2 张、3 张图片延迟；
- ROI 图片与全景图片大小；
- 超时率；
- 重试次数；
- JSON 合法率；
- 每事件 Token 用量；
- 每事件图片上传字节数；
- 事件防抖前后请求量；
- 规则路由前后请求量；
- API 断开时 Pipeline 是否稳定；
- API 恢复后的处理策略。

### 17.3 优化效果指标

最终至少输出：

```text
大模型调用次数降低比例
平均每事件 Token 降低比例
平均每事件图片上传量降低比例
普通事件平均响应时间
高风险事件平均响应时间
本地告警首次显示延迟
大模型首 Token 延迟
大模型完整结果延迟
```

### 17.4 Agent

构造：

```text
单路 FPS 下降
单路断流
GPU 温度高
队列积压
API 超时
API 返回非法 JSON
模型建议不存在的工具
模型参数越界
工具执行失败
执行后状态未恢复
```

验证系统是否正确拒绝、降级、恢复、回滚和记录。

---

## 18. 风险与应对

| 风险 | 影响 | 应对策略 |
|---|---|---|
| YOLO11s 四路负载过高 | FPS 下降、温度升高 | infer interval、关闭 OSD、YOLO11n 降级 |
| YOLO11 输出解析错误 | 框位置或类别错误 | 基于实际输出验证，不直接套旧解析器 |
| API 延迟较高 | 事件分析积压 | 有界队列、优先级、异步执行、连接复用 |
| API 不可用 | 多模态分析失败 | 本地规则继续工作，标记 unavailable |
| 事件重复触发 | 成本增加、队列爆满 | 去重、冷却、合并和限流 |
| 图片发送过多 | 延迟和费用增加 | 默认 1 张、ROI 裁剪、相似帧过滤 |
| Token 过多 | 费用和返回延迟增加 | 日志聚合、短 JSON、限制 max tokens |
| Kimi 和 DeepSeek 串行 | 总延迟增加 | 独立任务并行，按依赖关系串行 |
| 模型返回非法工具 | 安全风险 | 白名单、Schema 和参数范围校验 |
| API Key 泄露 | 账号和费用风险 | 环境变量、日志脱敏、Git 检查 |
| Jetson 8GB 内存不足 | OOM 或 Pipeline 崩溃 | 避免本地大模型常驻、控制队列和缓存 |
| Agent 连续错误操作 | 系统不稳定 | 冷却、确认、验证和回滚 |

---

## 19. 项目最终展示

### Demo 1：四路实时检测

展示：

- 四路视频；
- YOLO11s 检测；
- Tracker ID；
- infer interval 动态变化；
- 每路 FPS；
- GPU、温度和功耗。

### Demo 2：低成本事件理解

展示：

- 行人进入 ROI；
- 本地规则生成事件；
- 默认只抽取 1 张 ROI 关键帧；
- Kimi 输出场景描述和风险等级；
- 对比发送全景图和 ROI 图的延迟及大小。

### Demo 3：智能诊断

展示：

- 人为制造某一路 FPS 下降；
- Metrics 聚合结构化异常；
- 只调用 DeepSeek，不调用 Kimi；
- 输出调整 infer interval 的候选计划。

### Demo 4：低延迟响应

展示：

- 本地告警立即出现；
- 界面显示 AI 分析中；
- 大模型结果异步补充；
- 比较优化前后的用户感知延迟。

### Demo 5：受控 Agent

展示：

- 策略模块校验候选工具；
- 执行低风险操作；
- 检查 FPS 是否恢复；
- 记录完整审计日志；
- 对非法工具或越界参数进行拒绝。

---

## 20. 可用于简历的项目描述

### 项目名称

**JetEdge-Agent｜基于 Jetson 的多路视频实时感知与多模态智能运维平台**

### 项目描述

基于 Jetson Orin Nano 8GB 开发多路视频边缘 AI 平台，使用 C++、DeepStream、GStreamer 和 TensorRT 构建四路硬件解码、YOLO11s FP16 检测、目标追踪及事件分析 Pipeline；通过 infer interval、Tracker 补帧和模型降级策略优化端侧算力占用，实现 FPS、延迟、GPU、温度和功耗监控；设计事件去重、聚合和分级路由机制，仅对存在视觉歧义的事件调用 Kimi，多路系统异常只调用 DeepSeek，并通过 ROI 裁剪、关键帧过滤、非思考模式、连接池和异步队列降低 Token、图片上传量和大模型响应延迟；实现具备白名单校验、参数约束、执行验证、审计与回滚能力的本地 Agent。

### 可量化结果占位

最终根据实测替换：

```text
四路总吞吐量：待测 FPS
P95 端到端延迟：待测 ms
连续稳定运行：待测小时
FP16 相比基线提升：待测 %
事件 API 请求降低：待测 %
平均 Token 用量降低：待测 %
关键帧上传量降低：待测 %
本地告警首次显示延迟：待测 ms
故障恢复时间：待测秒
```

不得在测试完成前填写虚假性能数字。

---

## 21. 当前立即执行

阶段 3 已完成并验收通过：

```text
[x] 在本机创建隔离 Python 环境
[x] 安装 Ultralytics、ONNX 和 ONNX Runtime
[x] 导出 yolo11s.onnx
[x] 检查输入输出节点
[x] 执行 ONNX Runtime 推理
[x] 生成 model_info.txt
[x] 计算 SHA256
[x] 将 ONNX 复制到 Jetson
[x] 比对 Windows 与 Jetson 两端 SHA256
[x] 提交脚本、说明和报告到 GitHub
```

当前只准备在 Jetson 上进入阶段 4：

```text
TensorRT FP16 Engine
      ↓
nvinfer
      ↓
单路 YOLO11s 检测验证
```

不要提前生成阶段 5～8 的大量未经验证代码。

---

## 22. 阶段验收总表

| 阶段 | 核心产出 | 验收条件 |
|---|---|---|
| 3 | YOLO11s ONNX | Checker、ORT、SHA256 均通过 |
| 4 | TensorRT + nvinfer | 单路检测框正确且稳定退出 |
| 5 | 四路 + Tracker + Metrics | 四路连续稳定运行 30 分钟 |
| 6 | Event + Router + Keyframe | 去重路由有效，默认 1 张关键帧 |
| 7 | DeepSeek + Kimi | 异步、连接复用、失败降级和低消耗策略有效 |
| 8 | Agent Tools | 白名单、验证、审计、拒绝和回滚均有效 |

---

## 23. 最终结论

本方案采用“边缘实时感知 + 本地规则决策 + 云端按需理解 + 本地受控执行”的分层架构：

```text
Jetson + YOLO11s：
承担稳定、持续、低延迟的视频感知。

Tracker + Event Engine：
承担目标时序关系、事件发现、去重和本地快速判断。

Event Router：
决定是否需要调用大模型，以及调用哪个模型。

Kimi K2.6：
只承担低频、有歧义的视觉事件理解。

DeepSeek V4 Flash：
只承担聚合指标分析、复杂诊断和候选操作规划。

本地 Agent：
承担权限校验、工具执行、结果验证和审计回滚。
```

低消耗、低延迟的关键原则为：

```text
能不用大模型就不用
能只调用一个模型就不调用两个
能发送一张图就不发送三张
能使用非思考模式就不使用思考模式
能本地立即返回就不等待云端结果
能并行处理就不固定串行
```

该方案既避免了 Orin Nano 8GB 上常驻大模型带来的内存和实时性压力，也通过事件路由、图片压缩、Token 控制、HTTP 连接复用和异步执行降低了云端模型的调用消耗和返回延迟。最终项目重点不是“调用了两个大模型接口”，而是构建一套具有实时视频链路、低成本事件驱动、可控大模型增强、安全工具执行和可验证优化结果的完整边缘智能系统。
