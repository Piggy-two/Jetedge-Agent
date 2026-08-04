# Stage 12 — Agent 阶段验收报告(白名单工具调用 + 验证 + 审计 + 自动回滚)

**验收状态:通过(Jetson 实机,2026-08-04)**

## 1. 阶段目标(implementation_plan.md §62-65)

最小但完整的 Agent 目标驱动闭环:

```text
用户目标 → 查询指标 → 选择工具 → 保存基线 → 修改配置 → 固定时间验证
→ 达标保留 / 不达标回滚 → 输出报告
```

单一目标场景:「保证 cam1 推理 FPS 不低于指定值的情况下,降低全局 P95 延迟」。

**P95 定义**:推理+跟踪段延迟(nvinfer sink pad → nvtracker src pad),不含解码与 RTSP 抖动。由服务器按 frame_num 配对实测。

## 2. 架构

- **C++ 侧**(`jetedge_server`,独立演进,不动实时链路):
  - `MetricsRegistry` 新增 per-frame 延迟环形跟踪:input probe → output probe 按 `frame_meta->frame_num` 配对;每流 ring 4096 条(≈136s@30fps,128 KiB 总量)+ pending 256(溢出/5s 超龄逐出计 `latency_evicted`,end 未命中计 `latency_desync`);全局单调 watermark 做测量窗口切分;时钟注入使单测确定性。
  - Control API 新增 **`POST /benchmark`** 受控测量窗口:`{"duration_s":5..120,"per_stream":[...]}`;只读测量不走快照/写锁;单飞(`benchmark_mu_`);走审计;`stopping_` 标志保证 `stop()` 不被最长 120s 窗口阻塞;窗口内帧数/FPS/drop 用前后两次计数差值、延迟用 watermark 切分、`global` 为跨流样本池真实合并;每流 `complete = frames_in >= max(5, duration*5)` 标记窗口停滞流。
  - `GET /metrics/summary` 每流新增 `lat_samples / lat_avg_ms / lat_p50_ms / lat_p95_ms / lat_p99_ms / lat_max_ms`。
- **Python Agent**(`agent/`,独立进程):7 个白名单工具经 loopback HTTP(127.0.0.1:8090)调用 Control API;DeepSeek(`deepseek-v4-flash`,函数调用)生成低频候选计划,确定性代码执行/验证/回滚;LLM 不可用/熔断/空候选 → 确定性默认策略(按优先级 rank 升序选 interval==0 的非目标流升档,每轮 ≤2 流);Agent 进程被杀不影响管线(所有状态由 C++ 服务端持有)。密钥从 `~/.jetedge/secrets.env` 读取,全程不打印不落盘。
- **安全双层**:服务端 §16 流程(参数校验→安全状态检查→快照→有界修改→审计→读回验证→失败自动回滚)不变;Agent 侧叠加 CRITICAL 预检、写前复查 scheduler、硬 deadline(600s,到点回滚)、基线快照回滚 + 读回比对。

## 3. 工具集(7 个,implementation_plan.md §64)

| 工具 | HTTP 端点 |
|---|---|
| get_system_metrics | GET /metrics/summary + /scheduler/state |
| get_all_stream_status | GET /streams |
| get_scheduler_state | GET /scheduler/state |
| set_infer_interval | POST /streams/<id>/infer-interval |
| set_stream_priority | POST /streams/<id>/priority |
| run_benchmark | POST /benchmark |
| rollback_config | POST /config/rollback |

## 4. 验证判定(validator.py,纯确定性)

- 数据完整:after 窗口任一流 `complete=false` 或全局 P95 缺失 → 不可信;
- 双阈值防噪声:全局 P95 需 **≤ before×0.85 且 ≤ before−8ms**;before < 20ms 视为已达标无需改动;
- cam1 保底:`input_fps ≥ min_fps×0.95`;
- drop 不显著恶化(> before+2pp 且 > 1% 判失败);
- 窗口内 scheduler 状态迁移 → 重测一次,仍迁移 → 判失败;窗口内进入 CRITICAL → 直接中止。

## 5. 单元测试

- C++:`test_metrics_registry` **29 checks**(精确分位数、ring 上限、pending 溢出/超龄、desync、since 窗口、watermark 跨流单调、乱序配对、snapshot 携带、双线程冒烟);`test_control_api` 扩展至 **255 checks**(FakeBackend 延迟注入、/metrics/summary 延迟字段、benchmark 正路径/负路径/审计行)。
- Python:`agent/tests/` **41 用例**(goal_parser 中英文变体、validator 全部判定规则、tool_registry 防御性校验、planner 清洗与确定性排序、deepseek 围栏剥离/重试/熔断、executor 保留/回滚双闭环)。
- 总计:**ctest 7/7 + 41 用例,0 失败**。

## 6. 实机验收(Jetson,4 路 RTSP)

### 6.1 端点实测

- `/metrics/summary` 延迟字段:4 流 lat_samples 1795-1982,P95 45.4-46.0 ms、P50 36.1、P99 56.8、max 91.7(cam2 单次 876ms 为一次性停滞,后续窗口消失);
- `/benchmark` 正路径(10s 窗口):1201 样本、global P95 43.5 ms、FPS 119.7、drop 0.00%、4 流 complete、scheduler NORMAL→NORMAL;
- 负路径:`duration_s=0/300/非数字` → 400 PARAM_DURATION;`per_stream=["cam9"]` → 404 PARAM_STREAM;
- **排队语义**:窗口运行期间 `GET /streams` 排队 9007ms 后正常响应(单 accept 线程,文档化行为);
- **实机发现并修复 1 个缺陷**:窗口边界效应(frames_out 差值 > frames_in,窗口开启时 tracker 内 1 帧在途)导致 drop_rate 无符号下溢成天文数字 → 改用有符号差值并 clamp 到 [0,1],复测 0.00%。

### 6.2 Agent 端到端场景

**场景 A — LLM 在线 + 未达标自动回滚**(`python3 agent/main.py --goal "保证 cam1 推理 FPS 不低于 15,降低全局 P95 延迟" --benchmark-duration 20`):

```text
before 20s 窗口: 全局 P95 = 43.3 ms
→ 基线快照 snap_..._118
→ LLM 在线调用(返回空候选 — deepseek-v4-flash 工具调用不稳定)
→ 确定性回退: cam3/cam4 interval 0→1
→ round 1 after:  P95 = 42.4 ms → 未达标(需 ≤36.8 且 ≤35.3)→ 自动回滚 ✓
→ round 2:        cam3/cam4 interval 0→2
→ round 2 after:  P95 = 41.7 ms → 未达标 → 自动回滚 ✓
→ 退出码 1,报告 + 审计完整
```

**LLM 故障不影响闭环**:LLM 空候选/失败时计划降级为确定性策略,执行/验证/回滚照常 —— 与 README 验收标准「Agent 或大模型故障不影响底层 Pipeline」一致。

**场景 B — 确定性 + FPS 保底触发回滚**(`--no-llm --goal "保证 cam1 推理 FPS 不低于 35,降低全局 P95 延迟"`):

```text
before 20s 窗口: 全局 P95 = 44.0 ms, cam1 推理 FPS 29.8
→ 确定性计划: cam3/cam4 interval 0→1
→ round 1 after:  P95 42.4 ms 且 cam1 FPS 26.1 < 保底 33.25(35×0.95)
   —— 提高低优流 interval 触发 batch 填充效应,cam1 推理 FPS 反降
→ 验证器双条件同时触发 → 自动回滚 ✓
→ round 2(interval 0→2):cam1 FPS 24.9 → 仍不达标 → 自动回滚 ✓
→ 退出码 1
```

此场景证明验证器在真实负载下拦截了「会损害目标流的变更」—— FPS 保底是有实际意义的。

**场景 C — 故障注入**:agent 运行中(30s benchmark 窗口内)SIGKILL:
- 管线 0 ERROR(`/errors/recent` count=0),4 流 RUNNING,服务器日志零异常;
- 已应用的未完成变更(cam2/cam4 interval=1)因 agent 被杀未及回滚而残留 —— 独立进程架构的固有权衡(回滚由 agent 执行),已记录并人工恢复。

### 6.3 双审计链

- Agent 端 `logs/agent/audit.jsonl`:start/observe/baseline_snapshot/benchmark/llm_candidates/plan/action/verify/rollback/abort/run_end 全阶段;
- 服务端 `logs/control/audit.jsonl`:benchmark×20、config_snapshot×7、rollback×12、set_infer_interval×25、set_priority×1 —— 全部走 §16 流程(快照+审计+读回);
- 两链通过 snapshot_id / request 引用可关联。

## 7. 实测数据汇总(全部来自真实运行)

| 指标 | 值 |
|---|---|
| ctest | 7/7(control_api 255 checks + metrics_registry 29 checks) |
| python unittest | 41/41 |
| 4 流 P95(P50/P99/max) | 45.4-46.0 ms(36.1 / 56.8 / 91.7) |
| 10s benchmark 窗口 | 1201 样本、global P95 43.5 ms、119.7 FPS、drop 0.00% |
| 场景 A before→r1→r2 | 43.3 → 42.4 → 41.7 ms(需 ≤36.8/35.3) |
| 场景 B cam1 FPS | 29.8 → 26.1 → 24.9(保底 33.25) |
| 故障注入 | 0 ERROR、4 流 RUNNING、退出码 1 |
| RSS / 退出 | 运行中 ~620 MiB 收敛;SIGINT 退出码 0 |

## 8. 遗留与后续

- **保留路径**(达标保留)在实机未演示:43.3 ms 基线 + 双阈值(15%/8ms)下 2 流 interval≤2 只能到 41.7 ms,目标不可达 —— 未达标回滚是正确行为;保留路径由单测覆盖(`test_goal_met_keeps_config`);
- Agent 常驻 watch 模式(本阶段仅单次运行);
- 多目标场景解析;
- restart_stream / get_recent_errors 工具接入 Agent(本阶段按 implementation_plan §64 用 7 工具);
- 审计 JSONL 轮转(服务端与 Agent 端均为追加型,属最终稳定性验收项);
- `/benchmark` 占用单 accept 线程期间其余 API 排队(文档化语义);
- agent 被杀时未完成变更残留(独立进程架构固有权衡,已记录)。
