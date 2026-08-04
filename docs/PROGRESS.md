# JetEdge-Agent Progress

最后更新时间：2026-08-02

## 当前结论

- 阶段 0（环境核查）：已完成 ✓
- 阶段 1（单路硬件解码）：已完成 ✓
- 阶段 3（YOLO11s ONNX 导出、验证、传输与 SHA256 一致性验收）：已完成 ✓
- 旧方案阶段 2（四路 streammux + fakesink）：已完成 ✓，代码已在新方案 Stage 5 中复用
- 阶段 4（TensorRT FP16 Engine + 单路 nvinfer 验证）：已完成 ✓（2026-08-01）
- **阶段 5（四路检测 + Tracker + 结构化 JSONL + per-stream Metrics）：已完成 ✓（2026-08-01）**
- **阶段 6（事件系统、事件去重和关键帧抽取）：已完成 ✓（2026-08-01）**
- **阶段 7（Qwen + DeepSeek 异步分析）：已完成 ✓（2026-08-01）**——mock/故障注入/线上真实 API 三层验收全部通过；线上验收修复 Qwen markdown 围栏解析缺陷并切换模型（qwen3.6-flash / deepseek-v4-flash）
- **阶段 8（RTSP 故障隔离与恢复）：已完成 ✓（2026-08-02）**——4 路 RTSP 冒烟（四路 10s 内 RUNNING、200s 零重连零失败）+ 10 轮 cam3 停/恢复故障注入（cam1/2/4 全程 0 stall / 0 reconnect / 0 failure，cam3 每轮自动恢复）+ FAILED 路径（6 次真实连续失败后重试停止）；本会话定位并修复 2 个缺陷：**watchdog tick 时间戳下溢假 stall**（now 循环开头捕获、前一流重建耗时后无符号下溢——cam4 每轮必误报）与**垂死 rtspsrc 陈旧错误重复计数**（健康流误 FAILED，元素身份校验修复）；ctest 4/4、事件 JSONL 8419 行 0 非法、RSS 收敛、EOS/Ctrl-C 干净。详见 `docs/stage8_rtsp.md`
- **阶段 9（确定性 C++ 动态调度器）：已完成 ✓（2026-08-02）**——纯逻辑状态机 NORMAL|PRESSURE|THERMAL|CRITICAL|RECOVERY（滞回/最小保持/冷却/调整预算 2/120s/热优先级/CRITICAL 不增载/缺失指标不困死），状态表 {0,0,0}/{0,1,2}/{0,2,3}/{1,3,15}/三级逐级恢复；只读系统采样（/proc/stat、/proc/meminfo、thermal zone 最大温度）；decoder src 探针逐流间隔 drop（计数先于丢弃，RTSP watchdog 看全速率）；优先级保护（cam1 high 最晚被节流）。验收：单测 54 checks + ctest 5/5；实机 Run A 正常负载零干扰 / Run B 6×yes 烧机 PRESSURE 精确节流（cam2 15.0、cam4 10.0 fps = 30/2、30/3）+ 逐级恢复 / Run C 真实温度 THERMAL→CRITICAL 预算封顶管道零影响 / Run D 闭环验证并发现调参规则（滞回间隙须 > 热噪声 ~0.5°C）；JSONL 0 非法、RSS 收敛、全部干净退出。详见 `docs/stage9_scheduler.md`
- **阶段 11（安全 Control API、配置快照、验证与回滚）：已完成 ✓（2026-08-04）**——白名单 HTTP Control API（自写 HTTP/1.1，零新依赖）；写操作统一流程（参数校验→安全门控 CRITICAL 拒升载→修改前快照→有界修改→审计→读回验证→失败自动回滚，写操作互斥串行）；快照 JSON 落盘（max 32 剪除）+ 回滚恢复全部字段；审计 JSONL（before/after/snapshot_id）；最近错误环形缓冲（bus ERROR/RTSP FAILED 喂入）；所有控制操作 g_main_context_invoke 派发到 GLib 主循环线程（有界等待、失败优雅降级）；运行时优先级 runtime_priorities_（调度 tier 映射改读它）。验收：单测 206 checks + ctest 6/6；实机 4 路 RTSP 全端点 curl（只读 6 类 + 写操作 7 类非法拒绝 + interval/priority/快照/回滚/restart 全过、cam4 interval=2 节流实机生效 ~80s、restart 恢复 RUNNING + 30s 节流、回滚恢复全部字段）；8080 被 Open WebUI 占用→CTL100 降级继续（API 故障不影响管道实机证据）；审计 6 条全 success；52,112 检测行 + 3,113 事件行 0 非法；RSS ~620 MiB 收敛；SIGINT 退出码 0。详见 `docs/stage11_control.md`

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

## 阶段 7 验收记录（2026-08-01）

### 代码与配置

- llm 模块：`llm_types.h / llm_config.h / request_queue.h / circuit_breaker / http_client / prompt_manager / llm_router`（见 `docs/stage7_llm.md` §2）
- 配置：`configs/streams_stage7.yaml`（llm 默认禁用；qwen3.6-flash / deepseek-v4-flash）
- 密钥：env 优先 + `~/.jetedge/secrets.env` 兜底，从不打印/落盘

### 实测结果（全部 Jetson 实机）

| 检查项 | 结果 |
|---|---|
| 单元测试 | test_event_engine + test_circuit_breaker + test_prompt_manager（20 项）：ALL PASS |
| llm 禁用回归 | 4 路 2072 帧 EXIT=0；事件 1194 行与 Stage 6 一致；0 条 llm 日志 |
| mock 端点全链路 | qwen 369 + deepseek 6 请求；375 行 analysis JSONL 逐行校验 0 失败；管道 FPS 无影响（44.09 vs 44.07）|
| 故障注入（死端点）| curl rc=7 重试 3 次退避；5 次失败熔断 OPEN；288 请求跳过；管道 EXIT=0 |
| 线上真实 API（最终）| qwen3.6-flash 真实请求成功（4381/9042 ms，http 200），记录可直接解析；deepseek-v4-flash 真实调用通过；0 条解析失败/熔断；管道 EXIT=0 |
| 线上验收修复 | qwen 返回 ```json markdown 围栏 → 新增 `strip_markdown_fence()` 剥离后校验/存储（根因经单次真实 curl 确认）|

详细报告：`docs/stage7_llm.md`。

## 阶段 8 验收记录（2026-08-02）

### 代码与配置

- `reconnect_policy`（纯逻辑状态机 + 37 checks 单测）、`RtspConfig`、SourceBin rtsp 分支与 `rebuild_source`（sink_<idx> 映射稳定）、pipeline 1s watchdog + bus 错误分流 + FPS 验证窗口、`scripts/rtsp_serve.sh` + `configs/streams_stage8.yaml`
- 测试环境：MediaMTX v1.19.3（`~/jetedge-rtsp/`，用户目录无系统包）

### 实测结果（全部 Jetson 实机）

| 检查项 | 结果 |
|---|---|
| ctest | 4/4 PASS（含 test_reconnect_policy 37 checks）|
| 4 路冒烟（200s）| 四路 ~10s 内 RUNNING；0 重连 / 0 失败；每路 ~29.3 fps；事件 6773 行 0 非法；SIGINT 干净 |
| 10 轮 cam3 故障注入 | cam3 每轮 stall→1 失败→恢复→failures 归零（10/10）；**cam1/2/4 全程 0 stall / 0 reconnect / 0 failure**（460s）|
| FAILED 路径 | 6 次真实连续失败 → FAILED → 重试停止；发布端恢复后不自动复活（按设计）|
| 陈旧错误 | 元素身份校验 19 次正确忽略（早期运行）；真实 404 正确计数 |
| 事件 JSONL | 8419 行逐行校验 0 非法 |
| RSS / 退出 | 616.4 → 650.5 MiB 收敛；EOS/Ctrl-C 干净 |

### 本会话修复的缺陷

1. **watchdog 下溢假 stall（R1）**：`now` 在 tick 循环开头捕获一次，前一流（cam3）重建耗时 ~100ms 后，cam4 的检查用旧 `now` 减新的 `last` → 无符号下溢 → 每轮必假 stall。修复：`now` 移入每流循环内 + `last <= now` 保护。证据：插桩日志 `last=10957559 now=10957523` + nvstreammux 调试证实 source 3 帧全程 30fps（假 stall）。
2. **陈旧错误重复计数（R2）**：垂死 rtspsrc 的 2~4 条 bus ERROR 在重建后到达（不在 pending 窗口）→ 每条计一次失败 → 健康流 6 次失败误 FAILED。修复：`SourceBin::is_chain_element` 元素身份校验，旧元素错误忽略。

详细报告：`docs/stage8_rtsp.md`。

## 后续阶段

- ~~Stage 7：Qwen + DeepSeek API、异步队列和降级策略~~ → 已完成，见 `docs/stage7_llm.md`
- ~~Stage 8：RTSP 故障隔离与恢复~~ → 已完成，见 `docs/stage8_rtsp.md`
- ~~Stage 9：确定性 C++ 动态调度器（NORMAL | PRESSURE | THERMAL | CRITICAL | RECOVERY，含自适应推理间隔）~~ → 已完成，见 `docs/stage9_scheduler.md`
- ~~Stage 10：ftrace / CPU Affinity 分析~~ → 已完成，见 `docs/stage10_ftrace.md`
- ~~Stage 11：Control API、快照和回滚（Agent 前置）~~ → 已完成，见 `docs/stage11_control.md`
- Agent 白名单工具调用、验证、审计和回滚（含 run_benchmark 端点）
- INT8 PTQ 与精度回归
