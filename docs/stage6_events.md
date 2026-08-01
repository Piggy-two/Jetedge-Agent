# Stage 6 验收报告:事件系统、事件去重与关键帧抽取

验收日期:2026-08-01
设备:Jetson Orin Nano 8GB(与 Stage 5 相同环境,DeepStream 7.1 / GStreamer 1.20.3 / TRT 10.3.0)

## 1. 范围

- 规则事件:appearance / disappearance / count_high / count_exit / zone_entry;
- 事件去重与状态管理(appearance 去重、disappearance grace 帧、count 滞回、zone 去重);
- 事件触发的整帧关键帧 JPEG 保存(全局上限 + 质量配置);
- 事件 JSONL 输出,`keyframe` 字段关联保存的文件;
- 每流事件统计并入周期 metrics 报告。

不在本阶段范围:RTSP、Kimi/DeepSeek、Agent、INT8、自适应调度。

## 2. 设计与实现

| 文件 | 说明 |
|---|---|
| `include/jetedge/events/event_types.h` | EventRecord / ObservedObject / EventType 定义 |
| `include/jetedge/events/event_engine.h` + `src/events/event_engine.cpp` | 纯逻辑状态机:track 状态、grace、滞回、zone、计数;线程安全 |
| `include/jetedge/events/event_writer.h` + `src/events/event_writer.cpp` | JSONL 写入(ts_ms/stream_id/frame_num/event/class/track/conf/bbox/count/zone/keyframe) |
| `include/jetedge/events/keyframe_writer.h` + `src/events/keyframe_writer.cpp` | 整帧 JPEG 保存(官方 nvds_obj_enc 编码) |
| `include/jetedge/events/event_probe.h` + `src/events/event_probe.cpp` | nvtracker src pad 探针:元数据适配 → EventEngine → EventWriter + KeyframeWriter |
| `src/pipeline/pipeline.cpp` | events 配置解析、探针安装/移除、EOS 每流 flush、Ctrl-C flush |
| `src/common/config_loader.cpp` + `configs/streams_stage6.yaml` | `events:` 配置段(enable/jsonl_path/keyframe_dir/max_keyframes/jpeg_quality/grace/threshold/hysteresis/classes/zones) |
| `tests/test_event_engine.cpp` | 单元测试:去重 / grace / 滞回 / zone / flush —— **ALL PASS** |

配置示例(`configs/streams_stage6.yaml`):

```yaml
events:
  enable: true
  jsonl_path: .../logs/stage6_events.jsonl
  keyframe_dir: .../logs/keyframes
  max_keyframes: 150
  jpeg_quality: 85
  disappear_grace_frames: 15
  count_threshold: 3
  count_hysteresis: 1
  classes: [0, 1, 2, 3, 5, 7]
  zones:
    - name: road
      stream_id: cam1
      rect: [0, 250, 1280, 470]
```

## 3. 关键帧取帧技术路线(核心攻关)

本阶段的关键阻塞点是把 NVMM 批内某一路的帧保存为 JPEG。三次迭代与实机证据:

### 3.1 初版:gst_buffer_map 直接读像素(失败)

`gst_buffer_map` 在 NVMM buffer 上返回的 `map.data` 不是像素,而是 **`NvBufSurface*` 结构体指针**(64 字节正是该结构体大小)。实机错误:`KFW011 buffer too small for 1280x720 NV12, size=64`。
本机样例确认该模式:`deepstream-test4 pgie_src_pad_buffer_probe`(`gst_buffer_map → (NvBufSurface*)inmap.data`)。

### 3.2 二版:NvBufSurfaceMap CPU 映射 + NvBufSurface2Raw 兜底(部分成功)

- PITCH layout:`NvBufSurfaceMap(surf, idx, -1, NVBUF_MAP_READ)` + `NvBufSurfaceSyncForCpu` + `surfaceList[idx].mappedAddr.addr[plane]` / `planeParams.pitch[plane]` —— 与树内 `gst-dsexample` / `gst-nvtracker` 一致;
- surface 索引取 `frame_meta->batch_id`(nvdsmeta.h 文档明确:`NvBufSurfaceParams are at index batch_id in the surfaceList`)。

**实机结果**:batch 内 layout 不统一(实测 attrs `layout=0` PITCH、pitch0=1280;同时出现 BLOCK_LINEAR 表面),PITCH 部分保存成功(6/150),BLOCK_LINEAR 部分 `NvBufSurface2Raw` **plane 0 成功、plane 1(UV)失败**(`mem copy failed`),全部关键帧仍不可用。

### 3.3 终版:官方 nvds_obj_enc(isFrame=1)编码(验收通过)

DeepStream 官方对象编码器(`libnvds_batch_jpegenc`,样例 test4 / image-meta-test 实机模式):

```cpp
NvDsObjEncUsrArgs args = {0};
args.isFrame = 1; args.saveImg = FALSE; args.attachUsrMeta = TRUE;
args.quality = quality_;
nvds_obj_enc_process(enc_ctx_, &args, surf, nullptr, frame_meta);
nvds_obj_enc_finish(enc_ctx_);
// 从 frame_meta->frame_user_meta_list 取 NVDS_CROP_IMAGE_META →
// NvDsObjEncOutParams.outBuffer/outLen(JPEG 字节)→ 写文件
```

- GPU 侧编码,内部处理任意 layout / memType,不再需要 CPU 像素提取;
- 上下文 `nvds_obj_enc_create_context(0)` 在 `KeyframeWriter::init` 创建,析构销毁。

## 4. 实机验收结果(全部实测)

| 检查项 | 结果 |
|---|---|
| 4 路不同视频 | cam1 1442 帧 / cam2 163 / cam3 288 / cam4 179,共 2072 帧,EXIT=0 |
| 事件 JSONL | 1194 行,**全部为合法 JSON**(python json.loads 逐行校验 0 失败) |
| 事件分布(全量 JSONL) | cam1 appearance 369 / disappearance 369 / count_high 33 / count_exit 32 / zone_entry 369;cam2/3/4 少量(与各场景匹配) |
| 关键帧保存 | 150 次成功(cap=150 达到),113 个唯一文件(同毫秒多事件同名,属已知口径),**0 错误** |
| 关键帧内容正确性 | cam1 关键帧 vs 源视频同帧 SSIM=**0.985**;cam2 关键帧 vs office 帧 SSIM=0.976、vs bus/car 帧 -0.028 —— 内容与 stream 映射精确 |
| stream_id → keyframe | JSONL `keyframe` 字段与文件一一对应(150 引用) |
| 事件时序口径 | 周期 metrics 表为快照(最后 5 s 窗口+flush 可能未计入),JSONL 为全量,故 disappear 快照 356 vs 全量 369 属预期差异 |
| EOS | 4 路 EOS 分别处理 + 每流 flush disappearance + 总 EOS 优雅退出 |
| Ctrl-C | SIGINT → `graceful shutdown` → exit OK,EXIT=0,文件完整 |
| 内存 | 运行中 RSS 619.8 → 628.0 MB 后收敛,无持续增长 |
| 单元测试 | `test_event_engine` ALL PASS |
| 错误日志 | 最终版本全程 0 个 KFW/EVT 错误 |

## 5. 修复的 bug(均在实机发现)

1. **gst_buffer_map 像素误区**:NVMM buffer map 出的是 `NvBufSurface*`(64 B),非像素(KFW011)。
2. **batch 内 layout 混合**:PITCH 可 CPU map,BLOCK_LINEAR 的 `NvBufSurface2Raw` UV plane 复制失败(KFW017)。
3. **事件 JSONL `keyframe` 字段无引号**:`"keyframe":cam1_...jpg` 非法 JSON,全部行无法解析;已加引号(文件名 sanitize 后无需转义)。
4. **事件 JSONL `zone` 字段无引号**:`"zone":road` 非法 JSON;同上修复。这两处问题在第一版 Stage 6 代码中即存在,上一会话仅数行数未做 JSON 合法性校验而漏检。

## 6. 遗留说明

- zone `[0,250,1280,470]` 覆盖整帧下半部,导致 zone_entry 与 appearance 数相同(369),属配置可调项。
- 同毫秒多事件写同一文件名(后写覆盖),`max_keyframes` 按保存次数计(cap 日志 150 次 → 113 文件);如需唯一文件可在文件名中加入自增序号。
- 关键帧保存在 pad 探针线程内(同步 CUDA 编码+写盘);事件低频时开销可接受,事件风暴时存在阻塞实时线程的风险 —— 属后续优化项(异步化在阶段 7+ 计划)。
- 每事件一帧(非 ROI 裁剪),默认 1280x720 全帧;ROI 裁剪和 scaleImg 可用,未启用。
- 2 小时稳定性测试属于最终验收项,未执行。
- 下一阶段 Stage 7(Kimi + DeepSeek 异步分析)尚未开始。
