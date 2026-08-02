# Stage 9 报告：确定性 C++ 动态调度器

**验收日期**：2026-08-02（全部 Jetson Orin Nano 8GB 实机）

## 1. 目标与范围

确定性 C++ 调度状态机 `NORMAL | PRESSURE | THERMAL | CRITICAL | RECOVERY` + 按流优先级的自适应推理间隔（interval k = 每 (k+1) 帧推理一次）。调度完全由本地 C++ 逻辑决定，LLM 不参与实时决策。API 失败、网络、温度、负载均不影响管道存活——调度器只降负载，不停止管道。

每状态输出 per-priority 间隔表（high / normal / low）：

```text
NORMAL     {0, 0, 0}        全速率
PRESSURE   cpu ≥ 70%      {0, 1, 2}   增大推理间隔（Tracker 补偿）
THERMAL    temp ≥ 75°C    {0, 2, 3}   主要降低低优先级流
CRITICAL   temp ≥ 82°C    {1, 3, 15}  低优先级 ≈ 暂停；禁止增加负载
RECOVERY   逐级恢复       {0,1,2}→{0,0,1}→{0,0,0}→NORMAL
```

机制（CLAUDE.md §15 全部落地）：

- **滞回**：enter > exit（cpu 70/50；thermal 75/70；critical 82/76）
- **最小保持时间** min_hold（15s）：状态必须保持满时长才允许迁移；CRITICAL 进入例外（安全优先，立即响应）
- **冷却时间** cooldown（30s）：回到 NORMAL 后阻止快速再升级（防抖）
- **调整预算**：每 120s 窗口最多 2 次升级；降级永不阻塞
- **热优先级**：温度信号压过 CPU 信号（PRESSURE 中升温 → THERMAL）
- **CRITICAL 不增载**：表为最大值，唯一出口是 RECOVERY（只降不升）
- **缺失指标不困死系统**：温度读数消失时 CRITICAL 可退出，恢复过程不被缺失数据阻塞

## 2. 代码实现

| 文件 | 说明 |
|---|---|
| `include/jetedge/scheduler/scheduler_policy.h` + `src/scheduler/scheduler_policy.cpp` | 纯逻辑状态机（无 GStreamer 依赖）：迁移规则、滞回/保持/冷却/预算、状态表、RECOVERY 分级 |
| `include/jetedge/scheduler/system_metrics.h` + `src/scheduler/system_metrics.cpp` | 只读采样：/proc/stat（CPU%）、/proc/meminfo（RAM%）、thermal_zone*/temp（最大可读温度 + 来源 zone 名；本机 cv0-2 缺失自动跳过）|
| `tests/test_scheduler_policy.cpp` | 12 组用例 54 checks |
| `include/jetedge/pipeline/source_bin.h` + `src/pipeline/source_bin.cpp` | `infer_interval_` 原子 + decoder src 探针 drop：先计 watchdog 计数（全速率），再按间隔丢弃；`drop_counter_` 重建时归零（重建后首帧必保留，避免 live 模式 mux 首 batch 饿死）|
| `include/jetedge/pipeline/source_manager.h` + `src/pipeline/source_manager.cpp` | `set_infer_interval(idx, interval)`（SourceBin 对象在 RTSP 重建中存活，间隔跨重连保持）|
| `src/pipeline/pipeline.cpp` | 调度驱动：`g_timeout_add_seconds(sample_interval_sec)` tick → 采样 → policy.update → 按优先级映射各流间隔并应用；状态迁移/间隔变更结构化日志；周期报告含调度状态 |
| `src/common/config_loader.cpp` | `scheduler:` 段解析 + 全字段范围校验（enter>exit、critical>thermal、cpu∈(0,100)、temp∈[30,125] 等）|
| `configs/streams_stage9.yaml` | 4 路 RTSP + 调度器（cam1 high / cam2,3 normal / cam4 low）|
| `configs/stage9_debug_lowthresh.yaml` | 低阈值调试配置（真实读数验证 THERMAL/CRITICAL 用）|

**关键设计决策**：

1. **逐流 drop 位置 = decoder src pad 探针**：nvinfer sink pad 的 buffer 是整 batch（无法逐流选择丢弃）；decoder 探针每流独立、已有帧计数。计数先于丢弃 → RTSP stall 检测与重连 FPS 验证仍看到全速率（验证窗口不被调度器污染）。
2. **CRITICAL 低优先级用有界间隔 15（≈2fps）而非完全停帧**：完全停帧会使 live 模式 nvstreammux 收不到该流任何帧（首 batch 等待 / 空窗风险）；15 是"等效暂停"且保留首帧送达。
3. **优先级保护**：cam1 high 在 PRESSURE/THERMAL 中保持 interval 0；CRITICAL 才升到 1。
4. **批等待副作用（已文档化）**：固定 batch 的 mux 等待被节流的流凑批（40ms 超时），同批的其他流输入率被上限 ~25fps——这是减载的必然结果，不损害任何流。

## 3. 单元测试

`test_scheduler_policy`：**54 checks 0 失败**（ctest 5/5 PASS）。覆盖：初始 NORMAL / 滞回（cpu 60 不进入）/ PRESSURE 进入 / min_hold 阻止 / PRESSURE→RECOVERY 三级→NORMAL 全程 / cooldown 阻止再升级 / 热优先级（NORMAL 与 PRESSURE 两个入口）/ CRITICAL 立即进入 + 不增载不变量（表逐档 ≥ 前状态）/ 预算耗尽阻止升级但放行降级 / RECOVERY 中信号回归再升级 / 缺失指标不困死（全 -1 保持 NORMAL；CRITICAL 中温度消失可退出；CPU 缺失不阻塞恢复）/ 预算窗口滚动重置。

## 4. 实机验收（4 路 RTSP，MediaMTX + ffmpeg 发布循环）

### 4.1 Run A — 正常负载无回归（streams_stage9.yaml，100s）

| 检查项 | 结果 |
|---|---|
| 4 路 RTSP | ~10s 内全部 RUNNING，0 重连 / 0 失败 |
| 调度器 | **全程 NORMAL** table=[0 0 0]，adjustments=0/2（阈值未触发，无干扰）|
| 采样值 | cpu 29-37% / mem 58.6-59.1% / temp 53.2-56.8°C（zone gpu-thermal）|
| 事件 JSONL | 与 Stage 8 行为一致（管道无退化）|
| 退出 | SIGINT → "exit OK"，app_exit=0 |

### 4.2 Run B — 真实 CPU 压力注入（150s：30s 预热 → 65s 烧机 → 55s 恢复）

6 个 `yes > /dev/null` 烧机进程把聚合 CPU 推到 99-100%。

| 时刻 | 事件 | 证据 |
|---|---|---|
| t=30s | 烧机启动，下一 tick 即 PRESSURE | cpu=99.9% → table=[0 1 2]，adjustments=1/2 |
| t=30s+ | 间隔应用 | cam2/cam3(normal) interval=1、cam4(low) interval=2、**cam1(high) interval=0（优先级保护）**|
| 烧机期 | 精确节流 | cam2 帧率 15.0 fps（=30/2）、cam4 10.0 fps（=30/3），推断 delta 计数精确吻合；cam1 保持最高速率 |
| t=95s | 停烧机 | cpu 99.8→45.7% → RECOVERY stage0 [0 1 2] |
| t≈110s | 恢复 step1 | [0 0 1]（cam2/3 → 0）|
| t≈128s | 恢复 step2 | [0 0 0]（cam4 → 0）|
| t≈143s | NORMAL | 全速率回归 |
| 全程 | RTSP | 4 路 RUNNING 0 失败 0 重连（CPU 饱和下管道存活）|
| 退出 | SIGINT | app_exit=0 |

### 4.3 Run C — 真实温度 THERMAL/CRITICAL（debug 低阈值配置，240s）

| 检查项 | 结果 |
|---|---|
| t≈0s | 环境余热 53.8°C ≥ 46 → **THERMAL** table=[0 2 3]（cam4→3、cam2/3→2、cam1→0）|
| t≈46s | gpu-thermal 升至 56.1°C ≥ 56 → **CRITICAL** table=[1 3 15] |
| 预算 | adjustments 2/2 封顶（THERMAL+CRITICAL 两次升级，不再允许升级）|
| 降载效果 | cpu 29-33% → 24-27%（GPU 推理负载确实下降）；cam4 以 ~2fps 运行 |
| RTSP | 4 路全程 RUNNING **0 失败 0 重连 0 假 stall**（cam4 节流至 2fps 未触发 watchdog——解码器计数为全速率）|
| 退出 | SIGINT → app_exit=0 |

### 4.4 Run D — CRITICAL⇄RECOVERY 闭环 + 调参规则发现（debug 阈值 56/55.6，260s）

- 机制闭环验证：CRITICAL（56.1°C 进入）→ 节流降温 → 55.6°C 退出 → RECOVERY 逐级恢复 → 温度回升 → CRITICAL 立即再进入（安全设计：CRITICAL 进入不受 min_hold/预算限制）。每次迁移均阈值正确。
- **发现（调参规则）**：滞回间隙必须大于设备热噪声（本机 gpu-thermal 负载下 ~0.5°C）。56/55.6 的间隙（0.4°C）小于噪声 → 系统在边界抖动（每次迁移本身正确：进入立即、退出等 min_hold）。生产阈值 75/70、82/76（5-6°C 间隙）天然免疫。已写入 `stage9_debug_lowthresh.yaml` 注释与本文档。
- 全程 RTSP 0 失败；app_exit=0。

### 4.5 数据与资源

| 检查项 | 结果 |
|---|---|
| ctest | 5/5 PASS（scheduler_policy 54 checks）|
| 事件 JSONL | stage9: 2287 行 / debug: 2868 行，逐行 JSON 校验 **0 非法** |
| 检测 JSONL | 43123 / 48795 行，0 非法 |
| RSS | 631.9 → 633.1 MB（30s 间隔，与 Stage 5-8 收敛模式一致）|
| 编译 | 0 warning |

## 5. 缺陷与设计修正（本会话）

1. **CRITICAL 进入不应被冷却时间阻挡**（初版 kNormal 中 cooldown 检查先于 hot_critical）——安全优先迁移必须无条件放行；已修复并有测试覆盖。
2. **间隔变更日志旧值=新值**（`interval 1 → 1`）：赋值发生在日志前；调整顺序，日志先于赋值。

## 6. 遗留说明

- CRITICAL 低优先级为有界间隔 15（≈2fps），非完全停帧——完全停帧的 nvstreammux live 模式行为未实机验证，保持安全默认并文档化。
- 温度恢复路径（CRITICAL→RECOVERY）在实机通过 Run D 验证（闭环抖动场景）；稳态单次恢复（设备冷却超过滞回间隙）在阈值合理的配置下由单元测试 + Run B（PRESSURE 同路径）覆盖。本机主动散热使负载下 gpu-thermal 稳定在 ~55.5±0.3°C，无法在不停止管道的前提下制造 >1°C 的降温摆幅——如实记录。
- 2 小时稳定性测试、GPU 专项压力测试属于最终验收项（与 Stage 8 一致）。
- 下一阶段（按 README 路线）：ftrace / CPU Affinity 分析，或先做调度器配置快照与 Control API（Agent 前置）。README 路线将 ftrace 列为 Stage 10。

## 7. 验收结论

**验收通过（2026-08-02）。** 确定性状态机（滞回/保持/冷却/预算/热优先级/不增载/缺失指标不困死）54 checks 全过；实机四条证据链闭合：正常负载零干扰（A）、真实 CPU 压力精确节流 + 优先级保护 + 逐级恢复（B，帧率 15.0/10.0 fps 与设计完全一致）、真实温度 THERMAL/CRITICAL 进入 + 预算封顶 + 管道零影响（C）、CRITICAL⇄RECOVERY 闭环与调参规则（D）。调度器只降负载、从不危及管道：所有 run 中 4 路 RTSP 全程 0 失败 0 重连，EOS/SIGINT 干净，JSONL 全合法，RSS 收敛。
