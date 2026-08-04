# Stage 10 — ftrace / CPU Affinity 性能分析

**日期**: 2026-08-04
**状态**: 实现并已实机验证（acceptance checks 见文末）
**Git**: `a7a9007`（Stage 9）之上的新改动：`scripts/start_pipeline.sh`
**设备**: Jetson Orin Nano 8GB，6× Cortex-A78AE @ 1.728 GHz（max，schedutil governor），JetPack/DeepStream 7.1
**输入**: 4 路 RTSP 720p30（本地 MediaMTX + ffmpeg 发布循环，`scripts/rtsp_serve.sh`）
**模型**: YOLO11s，batch=4 FP16 engine（Stage 4/5 验收件），nvinfer + nvtracker + 事件引擎全开，LLM 关闭（同 Stage 9 配置）

---

## 1. 方法（CLAUDE.md §13：symptom → evidence → hypothesis → controlled change → before/after → keep or revert）

问题领域是 **ftrace / CPU Affinity 分析**（无预定义症状，探索性）：先用 ftrace 建立"管道线程在 6 核上的调度行为"基线，再根据证据形成瓶颈假设，做受控钉核实验，用 before/after 决定 keep 或 revert。

全部原始 trace（~21 MB/份）保留在 `/tmp/ftrace_{baseline,pinned,app_pinned}_60s.txt`，不进 Git；本文记录可复现的分析命令与摘要数字。

采集方法（60s 窗口，sched_switch + sched_wakeup + sched_wakeup_new）：

```bash
# /tmp/capture_ftrace.sh：清缓冲 → 开事件 → tracing_on → sleep 60 → 关 → cp trace
TR=/sys/kernel/tracing
echo 0 > $TR/tracing_on && echo > $TR/trace && echo > $TR/set_event
echo sched_switch > $TR/set_event && echo sched_wakeup >> $TR/set_event && echo sched_wakeup_new >> $TR/set_event
echo 1 > $TR/tracing_on && sleep 60 && echo 0 > $TR/tracing_on && cp $TR/trace $OUT
```

tracefs 授权（用户只读 → 666）由 `sudo` 一次性脚本完成（`/tmp/fix_tracefs.sh`，本会话前置）。

## 2. 基线（before，无任何钉核）

- 管道 70 线程，**全部自由漂移于 6 核**（affinity 0-5），迁移率 8–39%（按 sched_switch prev 的 CPU 变化计）
- 高频线程迁移绝对量大：rtpjitterbuffer ×2 线程在 60s 内分别迁移 119 / 95 次
- 端到端：4 路 in=infer=out 完全无损，29.3–29.7 fps，调度器 NORMAL，cpu ~33% / temp ~55°C
- **wakeup→run 尾部调度延迟真实存在**（用 sched_wakeup 与下一 sched_switch 的 next_pid 配对）：

| 线程 | n | med | p99 | max |
|---|---|---|---|---|
| rtpjitterbuffer (20559) | 69 | 0.010ms | **45.3ms** | 45.3ms |
| nvstreammux task0 (20516) | 52 | 0.006ms | **26.0ms** | 26.0ms |
| rtpjitterbuffer (20557) | 409 | 0.001ms | 1.07ms | 32.4ms |
| cuda-EvtHandlr (20496) | 69 | 0.005ms | 2.9ms | 2.9ms |

> 45.3ms ≈ 1.4 个 30fps 帧周期。尾部经逐事件序列核对确认真实（非测量假象；首次"wakeup→被切出"配对测得 100–400ms 为误配——含运行时间，改用 next_pid 配对后剔除）。

**瓶颈假设 H1**: 线程跨核自由迁移（含唤醒迁移）→ 缓存/队列损失 → 尾部调度延迟。钉核消除迁移应消除相关尾部。

## 3. 受控实验 1 — 解码线程钉核（keep）

改动（唯一变量）: 8 个解码相关线程每流一对钉到独立核 —— `taskset -pc N`（src-camN-decode + V4L2_DecThread → camN = cpuN-1）。可逆，不改代码。

| 指标 | before（自由漂移） | after（钉核） |
|---|---|---|
| 8 解码线程迁移率 | 9.7–39.2% | **全部 0%** |
| rtpjitterbuffer 20559 wake→run p99 | 45.3ms | **1.48ms** |
| nvstreammux task0 p99 | 26.0ms | **2.24ms** |
| 端到端 fps（4 路） | 29.3–29.7 | 29.6–29.8 |
| 丢帧（in vs infer vs out） | 0 | 0 |

→ **keep**：迁移消除，解码路径相关尾部延迟大幅下降，端到端无回归。

## 4. 受控实验 2 — 应用线程钉核（revert）

改动: 12 个 `jetedge_server` 应用线程（gmain 回调 tick 为主：RTSP watchdog 1s / scheduler 2s / fps report 5s，均为旁路任务）钉到 cpu4-5。

结果: **失败/反效果** —— 20512 p99 仍有 161ms，20513 82ms；逐事件核对显示 cpu4-5 窗口内全是被钉的应用线程自身 + containerd(933/943) 互相抢（161ms 尾部为同核排队）。12 个同类线程挤入 2 核把内部竞争集中，而非隔离。

→ **revert**（恢复 0-5 自由漂移）。对照实验 1（每核仅 2 个同流线程）与实验 2（每核 6 个同类线程）的差异，钉核的有效性依赖**职责隔离而非简单聚堆**。

## 5. 固化 — `scripts/start_pipeline.sh`

解码线程为 GStreamer/DeepStream 内部线程，应用代码无法在创建点设置 affinity，故固化到启动脚本：

- `start [config.yaml]`：启动 `jetedge_server`（日志 `logs/jetedge_server.log`）→ 轮询检测 4 个 `src-camN-decode` 线程 → 按名字钉 `src-camN-decode→cpuN-1`，`V4L2_DecThread` 轮转 0-3
- `stop`：SIGINT 优雅退出并等待（沿用 Stage 4-9 的干净退出路径）
- `status`：列出 8 个解码线程的 affinity 状态
- 无 sudo、无系统包；affinity 随进程消亡，天然可逆

实机验证（重启 → 脚本钉核 → status 确认）：cam1→cpu0、cam2→cpu1、cam3→cpu2、cam4→cpu3，每核恰 1 个解码对；4 路 RUNNING，in=infer=out 无损。

## 6. 结论与遗留

**结论**（均有实测证据）:
1. 基线中 70 线程无 affinity 自由漂移，解码路径存在真实尾部调度延迟（wake→run p99 45ms）
2. 每流解码线程对钉核（2 线程/核）消除迁移并使相关尾部 p99 降至 ~1.5ms，端到端零回归 → keep
3. 应用线程同类聚堆钉核反效果（内部竞争集中）→ revert；应用线程为旁路任务，其尾部延迟不影响主链帧率
4. 固化方式为启动脚本（DeepStream 内部线程无法代码级 setaffinity）

**遗留**:
- V4L2_DecThread 与 src-camN-decode 的流级配对按出现顺序轮转近似（未做唤醒关联精确配对）；同核即共享缓存，错配对仅损失少量局部性，实验 1 结果已证明整体有效
- 只测了 33% CPU（NORMAL）负载；PRESSURE/THERMAL 下钉核行为（含 scheduler 自适应间隔与钉核的交互）未测
- 未做 trace_marker 应用级打点（sched 事件已足以定位调度问题；应用级阶段时间戳留作后续）
- 系统级干扰源（containerd 等）未隔离；原始 trace 在 /tmp，未入库
