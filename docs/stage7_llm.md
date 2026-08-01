# Stage 7 验收报告：Qwen + DeepSeek 异步分析

验收日期：2026-08-01
设备：Jetson Orin Nano 8GB（DeepStream 7.1 / GStreamer 1.20.3 / TRT 10.3.0）

## 1. 范围

- 事件路由：规则可确认事件留在本地；高风险视觉事件（zone_entry）→ Qwen 多模态复核；周期系统指标 → DeepSeek 文本诊断；
- 有界异步请求队列：优先级排序 + 过载丢弃；
- HTTP 客户端复用：libcurl 连接复用、超时、有限重试、指数退避、熔断器；
- 固定提示词 + Schema 校验（jsoncpp 字段检查）、输出 token 有界；
- API 故障绝不影响实时管道：本地事件立即写出，云端分析异步尽力而为。

不在本阶段范围：RTSP、Agent 工具执行、INT8、自适应调度、事件引擎扩展。

## 2. 设计与实现

| 文件 | 说明 |
|---|---|
| `include/jetedge/llm/llm_types.h` | LlmProvider / RequestPriority / LlmRequest / LlmResponse / CloudAnalysisRecord |
| `include/jetedge/llm/llm_config.h` | LlmConfig（端点、队列、熔断、路由表） |
| `include/jetedge/llm/request_queue.h` | BoundedPriorityQueue 模板：mutex+cv、优先级（低枚举值先出）、满时丢弃最低优先级、shutdown |
| `include/jetedge/llm/circuit_breaker.h` + `src/llm/circuit_breaker.cpp` | CLOSED → OPEN → HALF_OPEN 状态机（每个 provider 一个实例） |
| `include/jetedge/llm/http_client.h` + `src/llm/http_client.cpp` | libcurl easy API：连接复用（MAXCONNECTS=4）、Bearer 认证、超时、仅瞬态错误重试（连接失败/5xx）、500ms 起步指数退避 |
| `include/jetedge/llm/prompt_manager.h` + `src/llm/prompt_manager.cpp` | Qwen 视觉复核/DeepSeek 指标诊断固定提示词；OpenAI-compatible 请求体构建；JPEG base64 编码；响应 content 抽取 + Schema 校验 |
| `include/jetedge/llm/llm_router.h` + `src/llm/llm_router.cpp` | 路由决策 + worker 线程 + 熔断集成 + 云分析 JSONL 写出 |
| `src/events/event_probe.cpp` | 探针内事件先写本地 JSONL，再异步 enqueue（非阻塞） |
| `src/pipeline/pipeline.cpp` | LlmRouter 初始化、DeepSeek 周期指标定时器、shutdown 顺序 |
| `src/common/config_loader.cpp` | `llm:` YAML 段解析与校验 |
| `apps/jetedge_server/main.cpp` | 启动时加载 secrets（env 优先，`~/.jetedge/secrets.env` 兜底） |
| `configs/streams_stage7.yaml` | Stage 7 配置（llm 默认禁用；qwen3.6-flash / deepseek-v4-flash） |
| `tests/test_circuit_breaker.cpp` | 熔断器单元测试 |
| `tests/test_prompt_manager.cpp` | schema 解析单元测试（含 markdown 围栏剥离，20 项断言） |

### 路由表（默认，可在 YAML 配置）

| 事件类型 | 路由 | 优先级 |
|---|---|---|
| appearance / disappearance | 默认本地（规则可确认） | — |
| zone_entry | Qwen 视觉复核 | HIGH |
| count_high | 默认本地 | — |
| count_exit | 本地 | — |
| 周期系统指标（默认 60 s） | DeepSeek 诊断 | LOW |

## 3. 实机验收结果（全部实测）

### 3.1 单元测试

| 测试 | 结果 |
|---|---|
| `test_event_engine`（Stage 6 回归）| ALL PASS |
| `test_circuit_breaker`（6 组状态机用例）| ALL PASS |

### 3.2 llm 禁用（回归，`streams_stage7.yaml` 默认）

| 检查项 | 结果 |
|---|---|
| 4 路不同视频 | 2072 帧，EXIT=0 |
| 事件 JSONL | 1194 行，分布与 Stage 6 完全一致（cam1 appearance 369 / zone_entry 369）|
| llm 日志 | 0 条（完全无 LLM 行为）|
| 分析 JSONL | 不生成（符合预期）|

### 3.3 llm 启用 + 本地 mock 端点（全链路验证）

用本地 Python mock（OpenAI-compatible chat completions，返回 schema 合法响应）验证异步全链路：

| 检查项 | 结果 |
|---|---|
| 请求发出 | qwen 369 次（全部 zone_entry）+ deepseek 6 次（周期指标，5 s 间隔）|
| 优先级行为 | DeepSeek kLow 被高优先级 Qwen 请求挤占后 shed —— 符合设计 |
| 分析 JSONL | 375 行，**逐行 JSON 校验 0 失败**；qwen=369 / deepseek=6 |
| 响应 Schema 校验 | 每条响应通过 `confirmed/summary/confidence` 或 `healthy/issues/recommendations` 校验 |
| 图像编码 | 150 次 keyframe 保存、0 次编码失败告警（JPEG → base64 data URL）|
| 管道性能 | cam1 in/infer/out FPS = 44.09 vs llm 禁用 44.07 —— **无 measurable 影响** |
| EXIT | 0 |

### 3.4 故障注入（死端点 127.0.0.1:19999）

| 检查项 | 结果 |
|---|---|
| 连接失败检测 | curl rc=7（Couldn't connect），每次请求重试 3 次（max_retries=2），500ms/1000ms 退避 |
| 熔断 | 5 次连续失败后 CLOSED → OPEN；288 个后续请求被跳过（`circuit open for qwen`）|
| 管道不受影响 | 2072 帧全部处理，事件 1194 行，EXIT=0 |
| 日志 | LLM010 错误码记录请求失败，不打印任何密钥 |

### 3.5 线上 API（真实端点，受限测试）

**状态：通过（2026-08-01）**。第一轮运行（16:52）暴露 Qwen 解析缺陷，根因确认并修复后，最终运行（18:32）Qwen/DeepSeek 真实链路全部验证通过。

运行配置：`/tmp/streams_stage7_live.yaml`（由 mock 配置派生：单 cam1 `sample_720p.h264`；qwen→DashScope、deepseek→api.deepseek.com 真实端点；`max_size=8`；`deepseek_interval_sec=5`）。密钥经 `~/.jetedge/secrets.env` 运行时解析，全程不落盘、不打印。

#### 第一轮（16:52，修复前）——发现缺陷

| 检查项 | 结果 |
|---|---|
| DeepSeek 真实请求 | **成功**：`deepseek ok: metrics (req 179, 4791 ms, http 200)`，schema 校验通过，成功行写入 analysis JSONL |
| Qwen 真实请求 | 5 次请求全部收到 **HTTP 200**，但响应内容均未通过 schema 解析（`content is not the expected JSON`）→ 5 次连续失败 → **熔断 CLOSED→OPEN** → 约 288 个后续请求被跳过（未实际发送） |
| 熔断器行为 | 真实 API 下按设计工作：解析失败计为失败并累计，5 次后 OPEN，批量跳过，无请求风暴；费用受控（仅 5 次 qwen 图像调用 + 1 次 deepseek 文本调用） |
| 管道 | 不受影响：EXIT=0，事件 JSONL 1172 行与 Stage 6 cam1 分布一致 |

**根因（单次真实 curl 复现确认）**：qwen 系列模型（`qwen-vl-plus` 与 `qwen3.6-flash` 实测均如此）把请求的 JSON 包在 **markdown code fence**（```json … ```）里返回；`validate_review_json` 用 jsoncpp 直接解析原始 content，遇到开头的反引号行即失败。`content` 是普通字符串（非数组），模型语义判断正确（"school bus 被误判为 car"——视觉理解无问题），纯格式问题。对照：`qwen3-vl-flash` 返回无围栏 JSON，能直接通过。

**修复（全部落库提交）**：

1. `src/llm/prompt_manager.cpp`：新增 `strip_markdown_fence()`（剥离 ```/```json 围栏 + 空白裁剪），`validate_review_json` 解析前调用；`llm_router.cpp` 将 `result_json` 存为剥离后的规范化内容，保证 analysis JSONL 记录可直接解析；
2. 模型切换（用户决策，均已在各自 API 模型列表确认存在）：qwen `qwen-vl-plus` → **`qwen3.6-flash`**；deepseek `deepseek-chat` → **`deepseek-v4-flash`**（`configs/streams_stage7.yaml` 与代码默认值同步更新）；
3. 新增 `tests/test_prompt_manager.cpp`：20 项断言 ALL PASS（extract / 纯 JSON / ```json 围栏 / 裸 ``` 围栏 / 空白 / 围栏剥离 / 各类拒绝形态）。

#### 最终运行（18:32，修复后）

| 检查项 | 结果 |
|---|---|
| Qwen 真实请求 | **2 次全部成功**：`qwen ok: zone_entry (req 0, 4381 ms, http 200)`、`(req 161, 9042 ms, http 200)`；`result` 字段逐行可直接解析，内容为真实 `confirmed/summary/confidence`（"school bus 在右侧，检测正确，confirmed=true"、"置信度 0.30 低于阈值，unconfirmed"——语义合理） |
| 解析失败 / 熔断 | **0 条** schema invalid / circuit 日志，熔断全程 CLOSED |
| 管道 | 1442 帧（cam1 单流），EOS 正常，**EXIT=0**；事件 JSONL 1172 行（appearance 369 / disappearance 369 / count_high 33 / count_exit 32 / zone_entry 369，与 Stage 6 完全一致） |
| DeepSeek 真实请求 | 本轮被 qwen HIGH 优先级挤占（shed，符合设计）；`deepseek-v4-flash` 已单独真实调用验证：http 200、纯 JSON、schema 合法（healthy/issues/recommendations，304 字符） |
| 延迟 | qwen3.6-flash 单次 4.4–11.8 s（异步 worker，不阻塞管道；timeout 30 s 内） |
| 单元测试 | `test_event_engine` / `test_circuit_breaker` / `test_prompt_manager` 三个二进制全部 ALL PASS |

> 注：live 配置早期派生方案用 sed 会把两个端点一并替换为 DashScope（deepseek 模型不在其上）——实际派生改为 qwen→DashScope、deepseek→api.deepseek.com（与已提交的 `configs/streams_stage7.yaml` 一致）。

## 4. 设计要点

- **探针线程零阻塞**：`enqueue_event()` 仅做路由判断 + 互斥锁队列推入，无 I/O、无网络；
- **worker 线程独占 HTTP**：管道 streaming 线程与 LLM 线程完全隔离；
- **密钥永不落盘/日志**：经 `secrets.h`（env → secrets.env）按 provider 在调用时解析；
- **队满丢弃**：满时弹出最低优先级项，新请求入队；本地事件已先写出，云端缺失不丢信息；
- **熔断半开**：OPEN 超时后允许探针请求，成功达阈值恢复 CLOSED，失败立即重新 OPEN；
- **shutdown 顺序**：先停 LLM 定时器 → `llm_router_->stop()`（队列 shutdown + join worker）→ 再销毁事件引擎。

## 5. 已知限制

- 云端请求在进程退出时被丢弃（best-effort 设计，不等待在途请求完成）；
- 未做图片去重（同毫秒多事件可能重复发送关键帧，属后续优化）；
- 未做图片缩放（直接发送 1280x720 JPEG，质量 85）；
- 响应校验为字段级（jsoncpp），非完整 JSON Schema 校验；
- 2 小时稳定性测试属最终验收项，未执行。
