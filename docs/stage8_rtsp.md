# Stage 8：RTSP 故障隔离与恢复（验收通过 2026-08-02）

## 1. 目标与范围

SourceManager 支持 RTSP 源，每流独立状态机 `OFFLINE → CONNECTING → RUNNING → DEGRADED → RECONNECTING → FAILED`，指数退避重连（1s→2s→4s→8s→15s 封顶），单流故障隔离（坏流不影响健康流），恢复后输入 FPS 验证，超过重试预算进入 FAILED 停止自动重试（防止重试风暴）。不重启进程作为恢复设计。确定性 C++ 调度器（NORMAL/PRESSURE/THERMAL/CRITICAL/RECOVERY）属于后续阶段，不在本阶段范围。

## 2. 测试环境（全部在用户目录，无 sudo / 无系统包）

| 项 | 说明 |
|---|---|
| MediaMTX v1.19.3 (linux arm64) | `~/jetedge-rtsp/mediamtx` 单文件二进制；`mediamtx.yml` rtspAddress :8554，paths cam1..cam4 |
| 发布端 | `scripts/rtsp_serve.sh`：ffmpeg `-re -stream_loop -1 -c copy -bsf:v h264_mp4toannexb -rtsp_transport tcp` 循环发布 4 路 H.264 |
| 源视频 | cam1=sample_720p.mp4、cam2=sample_office.mp4、cam3/4=预先 remux 的 mp4（mov 容器经 ffmpeg `-c copy` 发布 RTSP 会损坏 RTP，见 2026-08-01 日志 §5） |
| 应用配置 | `configs/streams_stage8.yaml`（4 路 RTSP + 推理 + tracker + events；llm 禁用）；`configs/stage8_single_cam1.yaml`（单路调试） |

## 3. 代码实现（本阶段全部写入）

| 文件 | 说明 |
|---|---|
| `include/jetedge/pipeline/reconnect_policy.h` + `src/pipeline/reconnect_policy.cpp` | 纯逻辑每流状态机（无 GStreamer 依赖）：状态转换、指数退避 base×2ⁿ 封顶 max、`max_retries` 次失败后 FAILED、成功重置计数 |
| `tests/test_reconnect_policy.cpp` | 7 组用例 37 checks：初始状态 / 转换 / 退避序列（1000→2000→4000→8000→15000→FAILED）/ 成功重置 / 小 max 封顶 / reason 记录 |
| `include/jetedge/pipeline/stream_config.h` | `RtspConfig`：enable / live_source / transport(tcp\|udp\|auto) / watch_timeout_sec / first_frame_timeout_sec / max_retries / backoff_base_ms / backoff_max_ms / verify_sec / min_fps / rtspsrc_latency_ms |
| `src/common/config_loader.cpp` | `rtsp:` 段解析 + 范围校验（含 transport 白名单）；`streams[].type` 仅允许 file/rtsp |
| `src/pipeline/source_bin.cpp` | rtsp 分支：rtspsrc（latency / drop-on-latency / protocols）→ 动态 pad 按 caps 选 H.264 → rtph264depay → h264parse → nvv4l2decoder；`teardown()` 可重建；`sync_state_with_parent()`；decoder src 帧探针（frame_count + last_frame_ts_ms） |
| `src/pipeline/source_manager.cpp` | `rebuild_source(idx)`：unlink → release mux request pad → teardown → 新链 → 重请求同名字 `sink_<idx>`（stream_id→pad index 映射稳定）→ link → sync |
| `src/pipeline/pipeline.cpp` | 每流 `RtspWatch`（policy + 时间戳）；1s watchdog tick（断流→DEGRADED→退避→重建；CONNECTING 首帧后 verify 窗口 FPS≥min_fps 才 RUNNING；FAILED 停止）；bus ERROR 按元素名 `src-<id>-*` 分流（流级重连 vs 致命退出）；重连后重装 EOS 探针；周期报告含 rtsp 状态；RTSP 模式跳过 PAUSED preroll；重建后 pipeline 非 PLAYING 自愈重拉 |
| `scripts/rtsp_serve.sh` | MediaMTX + 每路发布循环的启停脚本（全部产物在 `~/jetedge-rtsp/`，不入 Git） |
| `configs/streams_stage8.yaml` / `stage8_single_cam1.yaml` | 4 路 / 单路 RTSP 配置（transport: tcp） |

## 4. 验收结果（2026-08-02，全部 Jetson 实机）

### 4.1 编译与单元测试

- `cmake --build build -j2`：0 warning；ctest 4/4 PASS（event_engine / circuit_breaker / prompt_manager / **reconnect_policy 37 checks**）
- file 模式回归（Stage 6/7 配置）：4 路 2072 帧 EXIT=0，事件 1194 行与 Stage 6/7 完全一致，0 条 rtsp/llm 日志 → 无行为退化

### 4.2 4 路 120s 冒烟（修复后首次，PID 36685）

| 检查项 | 结果 |
|---|---|
| 4 路 CONNECTING → RUNNING | 全部在 ~10s 内进入 RUNNING（cam1 29.2 / cam2 30.0 / cam3 32.6 / cam4 32.0 fps 验证通过）|
| 200s 稳定性 | 全程 0 重连 / 0 失败 / 0 FAILED；每路 ~29.3 fps 稳定 |
| 事件 JSONL | 6773 行逐行校验 0 非法 |
| SIGINT | 优雅退出 exit OK |

### 4.3 故障注入验收（10 轮 cam3 停/恢复 + Phase 2 FAILED 路径，PID 55118）

```text
Phase 1：cam3 停 6s → 恢复，重复 10 轮（每轮触发真实断源 + 重连 + FPS 验证恢复）
Phase 2：cam3 长期停源（~115s）→ 6 次真实连续失败 → FAILED → 停止自动重试
```

| 检查项 | 结果 |
|---|---|
| cam3 每轮 | 断源 → 5s 后 DEGRADED → 退避 1s → 重建 → CONNECTING → 首帧 → verify 5s → RUNNING（failures 归零）；10/10 轮恢复 |
| **cam1 / cam2** | **全程 0 stall / 0 reconnect / 0 failure**（460s 不间断，~30fps）|
| **cam4** | **全程 0 stall / 0 reconnect / 0 failure**（460s 不间断，~30fps）——修复前每轮必假 stall |
| cam3 真实失败计数 | "Could not open resource" / "Not found"（发布端重启竞态）被正确计为真实失败，随后重试成功 |
| Phase 2 FAILED | 6 次连续真实失败 → `RTSP006 ... FAILED, automatic reconnect stopped`；此后 0 次重试（重试风暴停止）；发布端恢复后 cam3 保持 FAILED 不自动复活（按设计）|
| 陈旧错误抑制 | 早期运行 19 次正确忽略（元素身份校验）；本运行 0 次触发（无陈旧错误到达）|
| 事件 JSONL | 8419 行逐行校验 0 非法（cam1 8045 / cam2 118 / cam3 82 / cam4 174）|
| RSS | 616.4 → 650.5 MiB 收敛，无持续增长 |
| SIGINT | 优雅退出 exit OK（含 FAILED 态流）|

### 4.4 本会话修复的两个缺陷（2026-08-02 定位）

**缺陷 R1：watchdog tick 时间戳下溢 → 假 stall（对"重建后的下一个流"必然误报）**

- 症状：停 cam3 后 cam4 **必然** 在 1-2s 内报 DEGRADED（10/10 轮），随后无谓重建；cam1/cam2 从不误报
- 证据链：
  1. 纯 GStreamer 客户端（与 app 相同链路，无 muxer/nvinfer）消费 cam4，停 cam3 期间 NAL 速率恒定 ~630/2s，零跌落 → **服务器持续投递，问题在应用管道**
  2. nvstreammux:5 调试显示断源期间 muxer 正常推 batch size=3 的部分批次，且 **source 3 帧全程 30fps 到达 muxer**（stall 窗口内 60 帧/2s 无跌落）→ decoder 全程在输出 → **stall 是假的**
  3. 插桩 tick 日志实锤：`last=10957559 now=10957523` —— **last 比 now 大 36ms**，`now - last` 无符号下溢 → 巨数 → 假 stall
- 根因：`tick_rtsp_watch` 在**循环开始时**捕获一次 `now`，但循环体里对前一个流（cam3）执行 `do_reconnect` 重建（teardown/build/link/sync，耗时 ~100ms 的主线程重活），cam4 的检查在重建之后才执行：`now` 是重建前捕获的旧值，`last`（帧探针）却是重建后读到的更新值 → `now - last` 下溢
- 修复：`now` 移入**每流循环内**重新读取（`mono_ms()` 每流一次，开销可忽略）+ kRunning 检查加 `last <= now` 下溢保护（探针与 tick 的良性竞态也一并覆盖）
- 验证：修复后同样 10 轮故障注入，**cam4 0 stall / 0 reconnect / 0 failure**；早期"cam4 被 muxer 扰动"的全部现象消失（假 stall 触发的重建 churn 才是真实扰动源）

**缺陷 R2：垂死元素错误被计为新失败 → 健康流被累进 FAILED**

- 症状（修复前）：cam4 帧流健康（RECONNECTING 期间 frames 持续增长）却 6 次失败进 FAILED；单次真实故障吞掉 2-4 次重试预算
- 根因：teardown 中的旧 rtspsrc 会连发 2~4 条 bus ERROR（"Internal data stream error" / "Could not write to resource"），消息**异步**到达：重建完成（mark_connect → CONNECTING）后到达的错误不再处于 Fix B 的 pending 窗口 → 每条都计为新失败
- 修复：`on_bus_message` 对流级错误做**元素身份校验**（`SourceBin::is_chain_element`：错误源必须属于当前链实例，被重建替换的旧元素一律忽略，记 INFO 日志可审计）
- 验证：早期运行 19 次陈旧错误被正确忽略；真实 404 错误仍被正确计数并重试恢复

**此前会话已修复（本会话回归确认）**：Fix A（`first_frame_timeout_sec`=12s 与 stall 窗口分离——长 GOP 首帧等待，cam1 GOP 8.33s 实测）、Fix B（pending 重连窗口内失败信号合并）、Fix C（自愈日志状态名）、`tick_rtsp_watch` 悬垂引用（按值拷贝 sid）。

### 4.5 关键实测数据

- 断源检测：`watch_timeout_sec=5`（无帧 5s → DEGRADED）；恢复验证：`verify_sec=5`（首帧后 5s 窗口 FPS≥1.0 → RUNNING）
- 重连时间线（单轮）：断源 → +5s 检测 → +1s 退避 → 重建 → 首帧（~1s）→ +5s 验证 → RUNNING ≈ 断源后 ~12s 恢复
- FAILED 路径：6 次失败 ≈ 95s（5s stall 检测 + 5×(12s 首帧超时 + 退避 1/2/4/8/15s)）
- 事件系统在 RTSP 模式下无退化（8419 行合法事件；关键帧照常保存）

## 5. 遗留说明

- FAILED 状态不会自动复活（重试预算耗尽，需人工介入或重启应用）——按设计；文档与周期报告可区分
- cam3 每轮恢复时会遇到 1-2 次真实 404（发布端 `cam-start` 后 ~1s 内 rtspsrc 重连撞上 MediaMTX 路径未就绪）——真实失败正确计数，随退避重试恢复
- 2 小时稳定性测试、GPU/内存压力测试属于最终验收项，未在本阶段执行
- 调度器（NORMAL/PRESSURE/THERMAL/CRITICAL/RECOVERY）为下一阶段内容
- `auto.crt`（MediaMTX 自签测试证书，仓库根目录）为测试产物，不入 Git（已加 .gitignore）

## 6. 验收结论

**验收通过（2026-08-02）。** 代码全部编译通过、ctest 4/4、4 路冒烟 + 10 轮故障注入 + FAILED 路径全部实测通过；故障隔离成立（停 cam3 其余三路 0 影响）、重连自动恢复、重试风暴受控、EOS/Ctrl-C/内存干净。本会话定位并修复了 2 个新缺陷（watchdog 下溢假 stall、陈旧错误重复计数）与回归确认了此前 4 个修复。已按 CLAUDE.md 4.6 流程同步文档并提交。
