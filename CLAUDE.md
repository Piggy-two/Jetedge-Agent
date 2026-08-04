# JetEdge-Agent — Claude Code Rules

## Mandatory Current Rules

Before any work, read these files in order:

1. `CLAUDE.md`
2. `README.md`
3. `docs/PROGRESS.md`
4. `models/model_info.txt`

Current scope is Stage 11 preparation only: Control API, snapshots, validation, rollback (per CLAUDE.md §16; Agent stage follows after). No Agent tool execution, INT8, or event-engine extensions while Stage 11 is pending.

Stages 4-10 are complete and accepted (2026-08-01/04): single-stream inference (`docs/stage4` evidence in `docs/PROGRESS.md`), four-stream + tracker + metrics, events + keyframes (`docs/stage6_events.md`), Qwen + DeepSeek async analysis (`docs/stage7_llm.md`), RTSP fault isolation (`docs/stage8_rtsp.md`), deterministic adaptive scheduler (`docs/stage9_scheduler.md`), ftrace / CPU Affinity analysis (`docs/stage10_ftrace.md`).

Hard constraints:

- Do not run or recommend `sudo apt update`.
- Do not re-export the PyTorch model on Jetson.
- Do not build Jetson TensorRT Engine on Windows.
- Do not commit model weights, ONNX files, TensorRT Engine files, videos, secrets, API keys, `.env` files, large logs, or event images.
- Before every `git pull`, run `git status` and inspect uncommitted changes.
- Do not overwrite uncommitted changes.
- Do not force push.
- Do not generate large Stage 10 to Stage 12 code early.
- Do not add Agent, INT8, Control API, or event-engine extensions while Stage 10 is still pending.

Read this file before planning, syncing, editing, building, running, or debugging anything in this repository.
`README.md` contains the current stage snapshot and the cross-device workflow. Detailed plans and measured reports live under `docs/`.

This file defines permanent engineering, synchronization, model, safety, and verification rules.

## 1. Project Goal

Build a stable and measurable multi-stream edge AI platform on Jetson Orin Nano 8GB using C++17, GStreamer/DeepStream, TensorRT, YOLO11s, deterministic runtime control, low-frequency cloud model analysis, and a safe local Agent.

Approved development order:

```text
Environment audit and repository skeleton
→ Single local-video hardware-decode baseline
→ Host-side YOLO11s ONNX export and validation
→ Jetson-side TensorRT FP16 engine and single-stream nvinfer validation
→ Four-stream detection, tracking, structured metadata, metrics
→ Event engine and asynchronous keyframe extraction
→ Qwen visual review and DeepSeek text diagnosis
→ RTSP fault isolation and recovery
→ Deterministic adaptive scheduling
→ ftrace / CPU Affinity analysis
→ Safe Control API, snapshots, validation, rollback
→ Agent tool calling, audit, validation, rollback
```

The engineering story is:

> Under limited compute, memory, thermal, and power budgets, build a multi-stream video inference system that measures bottlenecks, recovers from failures, adjusts runtime strategies, uses cloud models only for low-frequency analysis, and allows an Agent to perform safe, verifiable, and reversible operations.

The real-time pipeline must not depend on Qwen, DeepSeek, Python, the network, or an Agent.

---

## 2. Current Approved Stage Snapshot

As of 2026-08-02, the following work is accepted:

```text
Jetson remote-development environment: complete
Single local-video hardware-decode baseline: complete
YOLO11s ONNX export on Windows: complete
ONNX Checker: PASSED
ONNX Runtime inference: PASSED
NaN/Inf check: PASSED
Transfer to Jetson: complete
Windows/Jetson SHA256 comparison: PASSED
TensorRT FP16 engine build: complete (21.81 MiB, SHA256 c6cc41d0...a82274a)
Single-stream nvinfer detection: complete (1440 frames, bus/car conf>0.9, EOS/Ctrl-C/memory verified)
Four-stream detection + nvtracker + JSONL + metrics: complete (batch=4, 2072 frames, EOS/Ctrl-C/memory verified)
Event engine + dedup + keyframe extraction: complete (1194 valid JSONL events, 150 keyframes, SSIM 0.985 content check)
Qwen + DeepSeek async analysis: complete (queue/circuit-breaker/schema, 375 mock + real API acceptance, docs/stage7_llm.md)
RTSP fault isolation and recovery: complete (per-stream state machine, 10-round fault injection, docs/stage8_rtsp.md)
Deterministic adaptive scheduler: complete (NORMAL|PRESSURE|THERMAL|CRITICAL|RECOVERY, 54 checks + 4 real runs, docs/stage9_scheduler.md)
```

Current model artifact:

```text
Model: YOLO11s
Input: 1x3x384x640
Output: 1x84x5040
Batch: 1
Dynamic shape: false
SHA256: 41abd2ff906712b41c60de9b7d5d5f09918e23a331d80cc0926071600fd3e078
Jetson path: /home/seeed/JetEdge-Agent/models/yolo11s.onnx
Metadata path: /home/seeed/JetEdge-Agent/models/model_info.txt
```

The only approved implementation stage now is:

```text
Stage 10:
1. ftrace / trace_marker / CPU Affinity analysis of the real pipeline (follow §13:
   symptom → evidence → bottleneck hypothesis → controlled change → before/after → keep or revert).
2. Report measured evidence under docs/; no optimization claim without before/after data.
```

Out of scope for the current stage:

- Agent tool execution;
- INT8;
- Control API, snapshots, rollback;
- custom CUDA post-processing;
- scheduler / RTSP / LLM / event-engine extensions;
- broad refactoring.

Do not mark Stage 10 complete until the acceptance checks run on the Jetson.

---

## 3. Target Device Is the Source of Truth

Never assume JetPack, L4T, CUDA, TensorRT, DeepStream, GStreamer, plugin properties, header paths, library names, model parser APIs, engine compatibility, or power-mode names.

Before using a Jetson-specific API:

1. Inspect the installed version on the current Jetson.
2. Inspect local headers and libraries.
3. Inspect plugin properties with `gst-inspect-1.0`.
4. Inspect installed DeepStream samples and sample configurations.
5. Prefer a locally installed working example over remembered APIs.
6. Record any mismatch between the plan and the real device.
7. Propose the smallest compatible solution.

Do not invent APIs, paths, commands, properties, output tensor formats, parser behavior, or configuration keys.

TensorRT engines are device- and environment-specific artifacts. Build them on the target Jetson.

---

## 4. Cross-Device Synchronization and Sources of Truth

The project uses two synchronization channels.

```text
Tracked source, scripts, configs, and Markdown
Windows or Jetson → Git → GitHub → other device

Models, engines, videos, raw traces, and large benchmark artifacts
Source device → SCP or rsync → target device
```

### 4.1 Source-of-truth responsibilities

- **GitHub** is the source of truth for tracked source code, scripts, configuration templates, `README.md`, `CLAUDE.md`, and reports.
- **Windows host** is the source of truth for PyTorch-to-ONNX export, ONNX Checker, ONNX Runtime validation, and the original `model_info.txt`.
- **Jetson** is the source of truth for installed NVIDIA software, TensorRT engines, DeepStream behavior, runtime logs, benchmark results, and acceptance evidence.
- Chat context is not a replacement for repository state or device evidence.

### 4.2 Required read order after synchronization

Before starting or continuing a stage, read and inspect in this order:

1. `CLAUDE.md`;
2. the current status and synchronization section in `README.md`;
3. the relevant current-stage document under `docs/`;
4. `models/model_info.txt` when model work is involved;
5. `git status`, current branch, and latest commit;
6. relevant local Jetson headers, plugins, samples, configs, and logs.

### 4.3 Git synchronization rules

Do not assume the local checkout is current.

Before any synchronization attempt:

```bash
git status
git branch --show-current
git log -1 --oneline
```

Only run `git pull --ff-only` when all of the following are true:

- the user explicitly requested synchronization or continuation from GitHub;
- the current branch is correct;
- the working tree is clean, or the user has explicitly decided how to preserve local changes;
- no untracked or modified file would be overwritten;
- the pull can be fast-forwarded safely.

If the working tree is not clean, stop before pulling and report the exact files and risk. Do not stash, reset, clean, rebase, merge, or discard changes automatically.

Do not commit or push unless explicitly requested — except the pre-approved stage-completion auto-sync flow in 4.6. When requested, show the files to be committed and use one focused commit.

### 4.4 Large artifact transfer rules

Never commit:

```text
*.pt
*.pth
*.onnx
*.engine
*.trt
*.mp4
*.h264
*.h265
raw traces
large logs
keyframes
secrets
```

Transfer large artifacts with SCP or rsync. After transfer:

1. verify the destination path;
2. list file size and ownership;
3. compute SHA256 on both devices;
4. compare the full hash;
5. record the verified hash in model or stage documentation.

Do not rebuild an accepted ONNX on the Jetson unless the user explicitly changes the export specification.

### 4.5 Documentation synchronization

Whenever a stage changes state:

- update the current status in `README.md`;
- update the relevant stage report under `docs/` when it exists;
- update `models/model_info.txt` only for real model metadata changes;
- distinguish accepted, implemented-but-unverified, blocked, and planned work;
- never mark a feature complete based only on generated code.

### 4.6 Stage-completion auto-sync (standing user request, 2026-08-01)

The user has pre-approved an automatic sync flow at the end of every stage. Once a stage's acceptance checks pass on the Jetson:

1. Update documentation: the status snapshot in `README.md`, `docs/PROGRESS.md`, `docs/development_log.md`, and the relevant stage report under `docs/`.
2. Run `git status`; stage only the files involved in this stage and show them.
3. Commit with one focused message from the suggested list in section 8.
4. Push to GitHub.
5. Report the commit hash and the resulting sync state.

This pre-approval does not override the safety rules: never overwrite unrelated uncommitted changes, never commit secrets or large artifacts, never force push; if the working tree contains unrelated changes, stop and ask first. "Automatic" waives the per-commit permission prompt for the files listed in step 2 only; it does not waive the checks above.

---

## 5. Hard Safety Rules

### System changes

- Never run or recommend `sudo apt update`.
- Do not run system upgrades or replace JetPack, CUDA, TensorRT, or DeepStream.
- Do not install system packages without explicit user approval.
- Prefer existing components and user-directory installation methods.
- Do not modify boot, kernel, NVIDIA, power, clock, fan, swap, governor, or network settings unless the current task explicitly requires it and the user approves.
- Do not modify proxy settings, shell startup files, SSH configuration, or network interfaces unless explicitly requested.
- Read-only environment inspection is preferred before any change.

### Destructive commands

Do not run or recommend destructive operations without explicit approval, including:

- broad `rm -rf`;
- formatting or repartitioning disks;
- deleting model or system directories;
- `git reset --hard`;
- `git clean -fd`;
- force push;
- overwriting user configuration without a backup;
- killing unrelated processes;
- removing existing engines or reports before verifying a replacement.

Before replacing a user-managed file, inspect it and preserve unrelated content.

### Secrets and private data

- Never print, echo, log, commit, or expose API keys, tokens, passwords, private keys, credentials, or complete authorization headers.
- Do not place secrets in source code or committed JSON/YAML.
- Use environment variables or ignored local files.
- Do not include secrets in screenshots, test output, command history examples, exception dumps, or model prompts.
- Do not log full Base64 image payloads.
- Do not upload arbitrary local files through an LLM client.

---

## 6. Stage Discipline

Implement only the stage explicitly requested by the user. Do not silently add later-stage components.

Examples:

- environment audit: no pipeline implementation;
- model export: no Jetson engine build on Windows;
- Stage 4 single-stream inference: no four-stream, tracker, event system, or Agent;
- local multi-stream stage: no automatic RTSP reconnection unless requested;
- before a stable measurable pipeline: no Agent code;
- before deterministic controls and rollback: no LLM-generated write operations;
- do not add VLM, RAG, multi-Agent, kernel modules, cross-camera ReID, custom CUDA post-processing, or custom GStreamer plugins unless explicitly requested.

If a task is broad:

1. select the smallest safe current-stage deliverable;
2. implement only that deliverable;
3. record later work as follow-up items;
4. do not create empty abstractions for future stages.

A stage is complete only after its acceptance checks pass on the Jetson and the actual results are documented.

---

## 7. Required Workflow

### Before editing

1. Read this file.
2. Read the current status and synchronization rules in `README.md`.
3. Read the relevant plan or report under `docs/`.
4. Inspect `git status`, branch, and latest commit.
5. Inspect relevant real-device headers, plugins, samples, configs, logs, and existing files.
6. Restate the exact task boundary.
7. Propose a small implementation plan.
8. List files to create or modify.
9. State assumptions and unresolved dependencies.
10. Preserve unrelated user work.

### During implementation

1. Make the smallest coherent change.
2. Preserve behavior outside the task scope.
3. Avoid unrelated refactors.
4. Reuse existing functionality instead of duplicating it.
5. Separate configuration from implementation.
6. Add explicit error handling and cleanup.
7. Keep queues bounded.
8. Add tests for pure logic modules when practical.
9. Do not represent planned behavior as implemented behavior.

### After implementation

1. Provide exact build commands.
2. Provide exact run commands.
3. Provide exact acceptance checks.
4. Run available safe checks on the Jetson.
5. Report actual output, not expected output presented as fact.
6. Report remaining risks and unverified assumptions.
7. Update relevant documentation.
8. Suggest one focused Git commit message.
9. State whether the work is accepted, implemented but unverified, blocked, or planned.

Generated code without Jetson-side verification is not complete.

---

## 8. Repository and Git Rules

- Develop and compile Jetson C++ code on the Jetson through VS Code Remote-SSH.
- Use C++17 and CMake.
- Use an out-of-source `build/` directory.
- Do not commit videos, model weights, ONNX models, TensorRT engines, raw traces, secrets, or large benchmark artifacts.
- Do not change Git identity automatically.
- Do not commit, push, pull, rebase, merge, reset, clean, stash, or switch branches unless explicitly requested and safe.
- Never overwrite unrelated uncommitted user changes.
- Do not create directories or abstractions for features that do not exist yet.
- Keep generated artifacts separate from tracked source.

Suggested commit granularity:

```text
chore: add environment audit tooling
feat: add single-stream hardware decode baseline
feat: add yolo11s onnx export workflow
feat: add tensorrt fp16 single-stream inference
feat: support four local video sources
feat: add tracking and structured metadata
feat: add per-stream metrics registry
feat: add event and keyframe pipeline
feat: integrate guarded llm clients
feat: add rtsp reconnect state machine
feat: add adaptive inference scheduler
feat: expose safe control api
feat: add agent validation and rollback loop
docs: sync stage completion and next-stage plan
```

---

## 9. C++ and GStreamer Engineering Rules

### C++

- Prefer RAII and deterministic ownership.
- Avoid raw owning pointers and detached threads.
- Avoid mutable global state unless documented.
- Make shutdown idempotent.
- Validate all external configuration.
- Use stable `stream_id` values for per-stream state.
- Keep application-managed queues bounded.
- Do not continue with uninitialized variables after parse or I/O failures.
- Check bounds before indexing vectors, arrays, tokenized fields, or tensor outputs.
- Use explicit error types or status values for recoverable operations.
- Preserve the original error context in logs.

### GStreamer / DeepStream

Before writing or changing a pipeline:

- inspect the relevant installed sample;
- inspect every relevant plugin with `gst-inspect-1.0`;
- confirm media container, codec, parser, demuxer, decoder, and memory path;
- confirm property names and valid values for the installed version;
- confirm requested-pad ownership and teardown requirements;
- confirm the actual TensorRT output and parser contract.

Build one layer at a time:

```text
source → demux/parse → hardware decoder → fakesink
→ nvstreammux → fakesink
→ inference → metadata
→ tracker / analytics
→ optional display or exporter
```

Rules:

- Use `fakesink` for baseline performance tests.
- Do not include OSD/display in pure inference benchmarks unless measuring overhead.
- Do not mix RTSP troubleshooting into the initial local-file stage.
- Release requested pads, remove probes, stop watches, and clean up bins during teardown.
- Handle `ERROR`, `EOS`, state-change failures, and Ctrl-C explicitly.
- Do not claim zero-copy behavior without verifying actual memory types and plugin behavior.
- Do not copy an old YOLO parser without validating the actual YOLO11 output tensor.

Recommended module boundaries:

```text
pipeline    source and GStreamer lifecycle
inference   model configuration, TensorRT, metadata parsing
tracking    tracker integration
analytics   ROI, line crossing, loitering logic
events      event state, deduplication, keyframes
llm         asynchronous typed clients and schema validation
metrics     counters, histograms, resource samples
scheduler   deterministic runtime state machine
control     validated operations, snapshots, rollback
common      logging, errors, configuration, utilities
```

---

## 10. Logging and Debugging Rules

Use structured logs with relevant fields:

```text
timestamp | level | module | stream_id | event_id | state | operation | error_code | message
```

Requirements:

- Do not swallow errors.
- Distinguish transient stream errors from fatal process errors.
- Include the affected stream and pipeline stage.
- Rate-limit repetitive errors where appropriate.
- Never log credentials, full authorization headers, full Base64 payloads, or private image content.
- Record request IDs and durations for external API calls without recording secrets.

For debugging, follow:

```text
symptom
→ evidence
→ direct failure point
→ root cause
→ affected scope
→ minimal fix
→ regression test
```

Do not hide root causes with only a catch-all exception, silent fallback, arbitrary default value, or unexplained retry loop.

---

## 11. Configuration Rules

- Put runtime settings in YAML or JSON, not hard-coded constants.
- Validate configuration at startup.
- Reject invalid critical fields with clear field names and valid ranges.
- Keep defaults conservative.
- Record the effective configuration in logs and benchmark metadata.
- Add configuration groups only when the corresponding feature is implemented.
- Never write real API keys into committed configuration.

Possible groups:

```text
streams | pipeline | inference | tracker | analytics | events | llm | metrics | scheduler | control | agent
```

Do not introduce `llm` or `agent` configuration files during Stage 4.

---

## 12. TensorRT and Model Rules

- Export and validate ONNX before TensorRT integration.
- Confirm input/output names, shapes, dynamic dimensions, and sample inference.
- Build TensorRT engines on the target Jetson.
- Treat engines as environment-specific generated artifacts.
- Do not commit `.onnx` or `.engine` files.
- Verify the accepted ONNX SHA256 before building.
- Record model, precision, batch, input shape, device, JetPack, CUDA, TensorRT, build options, workspace, warnings, engine size, and engine SHA256.
- Start with FP16.
- Do not start INT8 before the FP16 path is stable and measurable.
- INT8 requires calibration data and accuracy regression testing.
- Implement custom CUDA post-processing only if profiling proves it is a meaningful bottleneck.
- Never report speedup, accuracy, FPS, or latency without real measurements.
- Validate YOLO11 coordinate restoration, confidence interpretation, class mapping, NMS behavior, and boundary clipping with real frames.

For the current accepted model:

```text
Expected input: 1x3x384x640
Observed ONNX output: 1x84x5040
Expected SHA256: 41abd2ff906712b41c60de9b7d5d5f09918e23a331d80cc0926071600fd3e078
```

If the actual Jetson parser or TensorRT output differs, report the evidence before changing the design.

---

## 13. Metrics and Benchmark Rules

Do not optimize before a baseline exists.

Performance work must follow:

```text
problem symptom
→ metric or trace evidence
→ bottleneck hypothesis
→ controlled change
→ before/after comparison
→ keep or revert
```

Every benchmark must record:

- code/Git version;
- Jetson and software versions;
- power mode and relevant clock settings;
- input identifiers, codec, resolution, FPS, and stream count;
- model, precision, input size, batch, and engine SHA256;
- inference interval and tracker state;
- output mode and enabled features;
- warm-up and measurement duration;
- effective configuration.

Key metrics:

```text
input/decode/inference/output FPS
P50/P95/P99 pipeline latency
batch wait time and fill rate
queue depth and drop rate
CPU/GPU/RAM utilization
temperature and power
reconnect count
LLM first-token and total latency
LLM timeout, retry, token, and schema-validity rate
Agent execution, verification, and rollback duration
```

Rules:

- Warm up before measurement.
- Change one important variable at a time.
- Use the same input set for comparisons.
- Do not present one short uncontrolled run as a reliable optimization result.
- Keep raw large artifacts outside Git; commit only concise reproducible summaries.

---

## 14. Event and LLM Resource Rules

These rules apply only after the event and LLM stages are explicitly requested.

### Local-first routing

```text
Rule-confirmed event                  → local result, no LLM
Ambiguous visual event                → Qwen
System metric, log, or performance issue → DeepSeek
High-risk operation request           → analysis + local Policy + typed tool
```

### Qwen rules

- Default to one ROI keyframe.
- Add context or temporal frames only when necessary; maximum three images per event by default.
- Resize and JPEG-compress locally.
- Deduplicate near-identical frames.
- Prefer non-thinking mode for simple confirmation.
- Escalate reasoning only for complex, high-risk events.
- Do not send continuous unrelated video.
- Do not block the real-time pipeline or the initial local alert.

### DeepSeek rules

- Do not send large raw log dumps.
- Aggregate metrics, deltas, top errors, duration, and recent actions locally.
- Trigger on state changes, meaningful anomalies, or scheduled summaries.
- Keep fixed prompts and schemas stable and place dynamic data later.
- Prefer non-thinking mode for routine classification and diagnosis.
- Bound output tokens.
- Treat every model response as untrusted input.

### Latency and reliability rules

- Emit the local event immediately; mark cloud analysis as pending.
- Route to Qwen or DeepSeek by event type; do not call both by default.
- Run independent requests concurrently when useful.
- Reuse HTTP clients, connections, and TLS sessions.
- Initialize worker pools and templates before the first event.
- Use bounded priority queues.
- Apply timeout, limited retry, backoff, circuit breaking, and overload shedding.
- API failure must never terminate the video pipeline.

---

## 15. RTSP and Scheduler Rules

### RTSP

Add RTSP only after local-file pipelines are stable. Recovery must:

- maintain state per stream;
- isolate failures and preserve healthy streams;
- use bounded retries and backoff;
- record reason and retry count;
- stop retry storms after a threshold;
- verify input FPS after recovery;
- clean up pads, bins, probes, and state safely.

Suggested states:

```text
OFFLINE → CONNECTING → RUNNING → DEGRADED → RECONNECTING → FAILED
```

Do not restart the entire process as the final recovery design.

### Scheduler

The runtime scheduler must be deterministic C++ logic, not LLM output. It must include explicit states, thresholds, hysteresis, cooldown, minimum duration, bounded changes, priority protection, and gradual recovery.

Suggested states:

```text
NORMAL | PRESSURE | THERMAL | CRITICAL | RECOVERY
```

In `CRITICAL`, neither the scheduler nor the Agent may increase load.

---

## 16. Control API and Agent Rules

Add the Agent only after metrics, deterministic controls, snapshots, validation, and rollback work without an LLM.

The Agent may call only typed, allow-listed tools, for example:

```text
get_system_metrics
get_stream_status
get_all_stream_status
get_scheduler_config
get_recent_errors
set_stream_priority
set_infer_interval
restart_stream
run_benchmark
rollback_config
```

The Agent must never:

- execute arbitrary shell commands;
- edit arbitrary files;
- call `sudo`;
- directly manipulate DeepStream objects;
- bypass the C++ scheduler;
- disable all critical streams;
- increase load during thermal or critical state;
- claim success without verification;
- expose secrets or upload arbitrary local files.

Every write operation must:

1. validate types and ranges;
2. check the current safety state;
3. save a pre-change snapshot;
4. apply a bounded change;
5. write an audit record;
6. collect post-change metrics;
7. compare against the stated goal;
8. keep the change only if validation passes;
9. otherwise roll back automatically.

The real-time pipeline must continue if the Agent, Python process, network, Qwen, or DeepSeek fails.

---

## 17. Testing and Documentation

Use unit tests for configuration validation, tensor parsing helpers, metrics math, event deduplication, scheduler transitions, cooldown/hysteresis, snapshots, rollback, schema validation, and tool parameter validation.

Use integration tests for pipeline startup/teardown, single- and multi-stream behavior, metadata flow, runtime changes, RTSP recovery, external API failure, and Control API operations.

Use stress tests for repeated restarts, repeated configuration changes, queue overload, memory growth, thermal pressure, API timeouts, and two-hour stability.

Every test record must include:

```text
preconditions | command | expected observable result | actual result | pass/fail
```

Documentation must distinguish:

- accepted behavior;
- implemented but unverified behavior;
- blocked work;
- planned behavior;
- measured results;
- assumptions;
- known limitations.

Never present planned, estimated, simulated, or generated numbers as measured results.

---

## 18. Stage 4 Acceptance Checklist

Stage 4 is complete only when all applicable checks are supported by real Jetson evidence:

```text
[ ] Installed JetPack/CUDA/TensorRT/DeepStream/GStreamer environment recorded
[ ] Accepted ONNX SHA256 verified before build
[ ] TensorRT can parse the ONNX model
[ ] FP16 engine generated on the Jetson
[ ] Engine build command and warnings recorded
[ ] Input/output bindings recorded and consistent with the parser
[ ] Engine file size and SHA256 recorded
[ ] Single local video uses hardware decode
[ ] nvinfer loads the engine successfully
[ ] YOLO11 output parsing produces plausible boxes, classes, and confidence
[ ] Letterbox/coordinate restoration is validated
[ ] ERROR and EOS paths are handled
[ ] Ctrl-C shuts down cleanly
[ ] No obvious continuous memory growth during the acceptance run
[ ] Relevant documentation is updated
```

Do not add tracker or four-stream work merely to complete this checklist.

---

## 19. Definition of Done

A task is complete only when:

- only the requested scope was implemented;
- the code builds, or the exact blocking dependency is documented;
- safe acceptance checks ran on the Jetson;
- actual results are reported;
- error paths and cleanup are considered;
- configuration and documentation are updated where needed;
- no secret or large generated artifact was added to Git;
- synchronization state is clear;
- remaining limitations are stated honestly.

---

## Agent skills

### Issue tracker

Issues are tracked as GitHub issues via the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Default canonical labels: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context repo — one `CONTEXT.md` + `docs/adr/` at the repo root (created lazily by `/domain-modeling`). See `docs/agents/domain.md`.
