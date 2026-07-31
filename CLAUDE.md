# JetEdge-Agent — Claude Code Rules

Read this file before planning, editing, building, running, or debugging anything in this repository.
The detailed project roadmap lives under `docs/`. This file defines the permanent engineering and safety rules.

## 1. Project Goal

Build a stable and measurable four-stream edge AI inference platform on Jetson Orin Nano 8GB using C++17, GStreamer/DeepStream, and TensorRT.

Development order:

```text
Environment audit
→ Single local video hardware decode
→ Four local video streams and nvstreammux
→ TensorRT FP16 inference, tracking, metadata
→ Metrics and layered benchmarks
→ RTSP fault isolation and recovery
→ Deterministic adaptive scheduling
→ ftrace / CPU Affinity analysis
→ Safe Control API, snapshot, rollback
→ Agent tool calling, validation, rollback
```

The engineering story is:

> Under limited compute, memory, thermal, and power budgets, build a multi-stream video inference system that can measure bottlenecks, recover from failures, adjust runtime strategies, and allow an Agent to perform safe, verifiable, and reversible operations.

The Agent is not a chat UI, does not participate in per-frame decisions, and must not be required for the real-time pipeline to run.

---

## 2. Target Device Is the Source of Truth

Never assume JetPack, L4T, CUDA, TensorRT, DeepStream, GStreamer, plugin properties, header paths, library names, model parser APIs, or power-mode names.

Before using a Jetson-specific API:

1. Inspect the installed version on the current Jetson.
2. Inspect local headers and libraries.
3. Inspect plugin properties with `gst-inspect-1.0`.
4. Inspect installed DeepStream samples and sample configurations.
5. Prefer a locally installed working example over remembered APIs.

Do not invent APIs, file paths, commands, properties, or configuration keys.
If the installed environment differs from the plan, report the mismatch and propose the smallest compatible solution.

---

## 3. Hard Safety Rules

### System changes

- Never run or recommend `sudo apt update`.
- Do not run system upgrades or JetPack/CUDA/TensorRT/DeepStream replacements.
- Do not install system packages without explicit user approval.
- Prefer existing components and user-directory installation methods.
- Do not modify boot, kernel, NVIDIA, power, clock, fan, swap, governor, or network settings unless the current task explicitly requires it and the user approves.
- Do not modify proxy settings, shell startup files, SSH configuration, or network interfaces unless explicitly requested.

### Destructive commands

Do not run or recommend destructive operations without explicit approval, including:

- broad `rm -rf`;
- formatting or repartitioning disks;
- deleting system/model directories;
- `git reset --hard`;
- `git clean -fd`;
- force push;
- overwriting user configuration without a backup;
- killing unrelated processes.

Before replacing an existing user-managed file, inspect it and preserve unrelated content.

### Secrets

- Never print, echo, log, commit, or expose API keys, tokens, passwords, private keys, credentials, or complete authorization headers.
- Do not place secrets in source code or committed JSON/YAML.
- Use environment variables or ignored local files.
- Do not include secrets in screenshots, test output, or command examples.

---

## 4. Stage Discipline

Implement only the stage explicitly requested by the user.
Do not silently add later-stage components.

Examples:

- Environment audit: no pipeline implementation.
- Single-stream stage: no RTSP, tracker, scheduler, monitoring stack, or Agent.
- Local multi-stream stage: no automatic RTSP reconnection.
- Before a stable measurable pipeline: no Agent code.
- Do not add VLM, RAG, multi-Agent, kernel modules, cross-camera ReID, custom CUDA post-processing, or custom GStreamer plugins unless explicitly requested.

If a task is too broad:

1. Select the smallest safe current-stage deliverable.
2. Implement only that deliverable.
3. Record later work as follow-up items.

A stage is complete only after its acceptance checks pass on the Jetson.

---

## 5. Required Workflow

Before editing:

1. Read this file.
2. Read the relevant plan under `docs/`.
3. Inspect `git status` and the current repository.
4. Inspect relevant real-device headers, plugins, samples, configs, and logs.
5. Restate the exact task boundary.
6. Propose a small implementation plan.
7. List files to create or modify.
8. State assumptions and unresolved dependencies.

During implementation:

1. Make the smallest coherent change.
2. Preserve behavior outside the task scope.
3. Avoid unrelated refactors.
4. Reuse existing functionality instead of duplicating it.
5. Separate configuration from implementation.
6. Add explicit error handling and cleanup.
7. Add tests for pure logic modules when practical.

After implementation:

1. Provide exact build commands.
2. Provide exact run commands.
3. Provide acceptance checks.
4. Run available safe checks on the Jetson.
5. Report actual output, not expected output presented as fact.
6. Report remaining risks and unverified assumptions.
7. Update relevant documentation.
8. Suggest one focused Git commit message.

Generated code without Jetson-side verification is not complete.

---

## 6. Repository and Git Rules

- Develop and compile on the Jetson through VS Code Remote-SSH.
- Use C++17 and CMake.
- Use an out-of-source `build/` directory.
- Do not commit videos, model weights, TensorRT engines, raw traces, secrets, or large benchmark artifacts.
- Do not change Git identity automatically.
- Do not commit, push, rebase, merge, reset, clean, or switch branches unless explicitly requested.
- Never overwrite unrelated uncommitted user changes.
- Do not create directories or abstractions for features that do not exist yet.

Suggested commit granularity:

```text
chore: add environment audit tooling
feat: add single-stream hardware decode baseline
feat: support four local video sources
feat: add tensorrt fp16 inference
feat: add per-stream metrics registry
feat: add rtsp reconnect state machine
feat: add adaptive inference scheduler
feat: expose safe control api
feat: add agent validation and rollback loop
```

---

## 7. C++ and GStreamer Engineering Rules

### C++

- Prefer RAII and deterministic ownership.
- Avoid raw owning pointers and detached threads.
- Avoid mutable global state unless documented.
- Make shutdown idempotent.
- Validate all external configuration.
- Use stable `stream_id` values for per-stream state.
- Keep queues bounded when managed by application code.
- Do not continue with uninitialized variables after parse or I/O failures.
- Check bounds before indexing vectors, arrays, or tokenized fields.

### GStreamer / DeepStream

Before writing a pipeline:

- inspect the relevant local sample;
- inspect each plugin with `gst-inspect-1.0`;
- confirm media container, codec, parser, demuxer, decoder, and memory path;
- confirm property names for the installed version.

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
- Do not include OSD/display in pure inference benchmarks unless measuring their overhead.
- Do not mix RTSP troubleshooting into the initial local-file stage.
- Release requested pads, remove probes, stop watches, and clean up bins during teardown.
- Handle `ERROR`, `EOS`, and state-change failures explicitly.
- Do not claim zero-copy behavior without verifying actual memory types and plugin behavior.

Recommended module boundaries:

```text
pipeline    source and GStreamer lifecycle
inference   model configuration and metadata parsing
tracking    tracker integration
analytics   ROI, line crossing, event logic
metrics     counters, histograms, resource samples
scheduler   deterministic runtime state machine
control     validated operations, snapshots, rollback
common      logging, errors, configuration, utilities
```

---

## 8. Logging and Debugging Rules

Use structured logs with relevant fields:

```text
timestamp | level | module | stream_id | state | operation | error_code | message
```

Requirements:

- Do not swallow errors.
- Distinguish transient stream errors from fatal process errors.
- Include the affected stream and pipeline stage.
- Rate-limit repetitive errors where appropriate.
- Never log credentials.

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

Do not hide the root cause with only a catch-all exception, silent fallback, or arbitrary default value.

---

## 9. Configuration Rules

- Put runtime settings in YAML or JSON, not hard-coded constants.
- Validate configuration at startup.
- Reject invalid critical fields with clear field names and valid ranges.
- Keep defaults conservative.
- Record the effective configuration in logs and benchmark metadata.
- Add configuration groups only when the corresponding feature is implemented.

Possible groups:

```text
streams | pipeline | inference | tracker | metrics | scheduler | control | agent
```

---

## 10. TensorRT and Model Rules

- Export and validate ONNX before TensorRT integration.
- Confirm input/output names, shapes, dynamic dimensions, and sample inference.
- Build TensorRT engines on the target Jetson.
- Treat engines as environment-specific artifacts.
- Record model, precision, batch size, input shape, device, TensorRT version, and build options.
- Start with FP16 after the basic pipeline works.
- INT8 is optional and requires calibration plus accuracy regression testing.
- Implement custom CUDA post-processing only if profiling proves it is a meaningful bottleneck.
- Never report speedup or accuracy numbers without real measurements.

---

## 11. Metrics and Benchmark Rules

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
- model, precision, input size, and batch settings;
- inference interval and tracker;
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
```

Rules:

- Warm up before measurement.
- Change one important variable at a time.
- Use the same input set for comparisons.
- Do not present one short uncontrolled run as a reliable optimization result.

---

## 12. RTSP and Scheduler Rules

### RTSP

Add RTSP only after local-file pipelines are stable.
Recovery must:

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

The runtime scheduler must be deterministic C++ logic, not LLM output.
It must include explicit states, thresholds, hysteresis, cooldown, minimum duration, bounded changes, priority protection, and gradual recovery.

Suggested states:

```text
NORMAL | PRESSURE | THERMAL | CRITICAL | RECOVERY
```

In `CRITICAL`, neither the scheduler nor Agent may increase load.

---

## 13. Control API and Agent Rules

Add the Agent only after metrics, deterministic controls, snapshots, and rollback work without an LLM.

The Agent may call only typed, allow-listed tools, for example:

```text
get_system_metrics
get_stream_status
get_all_stream_status
get_scheduler_config
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
- increase load during thermal/critical state;
- claim success without verification.

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

The real-time pipeline must continue if the Agent, Python process, network, or LLM API fails.

---

## 14. Testing and Documentation

Use unit tests for configuration validation, metrics math, scheduler transitions, cooldown/hysteresis, snapshots, rollback, and tool parameter validation.

Use integration tests for pipeline startup/teardown, multi-stream behavior, metadata flow, runtime changes, RTSP recovery, and Control API operations.

Use stress tests for repeated restarts, repeated configuration changes, memory growth, thermal pressure, and two-hour stability.

Every test record must include:

```text
preconditions | command | expected observable result | actual result | pass/fail
```

Documentation must distinguish:

- implemented behavior;
- planned behavior;
- measured results;
- assumptions;
- known limitations.

Never present planned, estimated, or simulated numbers as measured results.
Create documentation only when the corresponding feature becomes real.

---

## 15. Default Initial Stage

Unless the user explicitly requests another stage, start with environment inspection only.

Allowed:

- inspect OS, L4T, JetPack, CUDA, TensorRT, DeepStream, GStreamer, compiler, CMake, Docker, storage, memory, and power-mode information;
- locate DeepStream samples and sample media;
- inspect required plugins;
- create a minimal repository skeleton;
- write `docs/environment_report.md`;
- propose the next stage.

Not allowed during this stage:

- package installation;
- system configuration changes;
- inference implementation;
- RTSP;
- tracker;
- monitoring services;
- scheduler;
- Agent;
- Docker deployment.

All environment conclusions must come from the current Jetson device.

---

## 16. Definition of Done

A task is complete only when:

- only the requested scope was implemented;
- the code builds, or the exact blocking dependency is documented;
- safe acceptance checks were run on the Jetson;
- actual results are reported;
- error paths and cleanup are considered;
- configuration and documentation are updated where needed;
- no secret or large generated artifact was added to Git;
- remaining limitations are stated honestly.
