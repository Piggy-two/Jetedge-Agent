#include "jetedge/control/control_server.h"

#include <algorithm>
#include <vector>

#include "jetedge/common/logging.h"
#include "jetedge/control/control_util.h"
#include "jetedge/control/param_validation.h"

namespace jetedge {
namespace control {

namespace {

// "/streams/cam1/infer-interval" → ["streams", "cam1", "infer-interval"].
std::vector<std::string> split_path(const std::string& path) {
  std::vector<std::string> parts;
  size_t pos = 0;
  while (pos < path.size()) {
    const size_t slash = path.find('/', pos);
    const std::string part = path.substr(pos, slash == std::string::npos
                                                   ? std::string::npos
                                                   : slash - pos);
    if (!part.empty()) {
      parts.push_back(part);
    }
    if (slash == std::string::npos) {
      break;
    }
    pos = slash + 1;
  }
  return parts;
}

std::string json_str(const Json::Value& v) {
  Json::FastWriter writer;
  return writer.write(v);
}

}  // namespace

bool ControlServer::start(const ControlConfig& cfg, ControlBackend* backend) {
  cfg_ = cfg;
  backend_ = backend;

  if (!snapshots_.init(cfg_.state_dir, cfg_.max_snapshots)) {
    return false;
  }
  if (!audit_.open(cfg_.state_dir + "/audit.jsonl")) {
    LOG_WARN("control", "audit log unavailable — write ops will not be audited");
  }

  HttpServer::Handler handler = [this](const HttpRequest& r) {
    return handle_request(r);
  };
  return http_.start(cfg_.host, cfg_.port, std::move(handler),
                     cfg_.max_body_bytes, cfg_.read_timeout_ms);
}

void ControlServer::stop() {
  http_.stop();
  audit_.close();
}

HttpResponse ControlServer::handle_request(const HttpRequest& req) {
  HttpResponse resp;
  try {
    resp = route(req);
  } catch (...) {
    // Safety net: route() handles all known failures; anything escaping is
    // still answered (never crashes the accept thread).
    resp = make_err(next_request_id(), 500, "INTERNAL", "unexpected control error");
  }
  return resp;
}

HttpResponse ControlServer::route(const HttpRequest& req) {
  const std::string request_id = next_request_id();
  const auto parts = split_path(req.path);

  if (req.method == "GET") {
    if (parts == std::vector<std::string>{"health"}) {
      return make_ok(request_id, json_health());
    }
    if (parts == std::vector<std::string>{"metrics", "summary"}) {
      return make_ok(request_id, json_metrics_summary());
    }
    if (parts == std::vector<std::string>{"streams"}) {
      return make_ok(request_id, json_stream_status("", nullptr));
    }
    if (parts.size() == 2 && parts[0] == "streams") {
      bool found = false;
      const Json::Value data = json_stream_status(parts[1], &found);
      if (!found) {
        return make_err(request_id, 404, "PARAM_STREAM",
                        "unknown stream '" + parts[1] + "'");
      }
      return make_ok(request_id, data);
    }
    if (parts == std::vector<std::string>{"scheduler", "config"}) {
      return make_ok(request_id, json_scheduler_config());
    }
    if (parts == std::vector<std::string>{"scheduler", "state"}) {
      return make_ok(request_id, json_scheduler_state());
    }
    if (parts == std::vector<std::string>{"errors", "recent"}) {
      return make_ok(request_id, json_recent_errors());
    }
    return make_err(request_id, 404, "PARAM_PATH", "unknown path '" + req.path + "'");
  }

  if (req.method == "POST") {
    Json::Value body(Json::objectValue);
    if (!req.body.empty()) {
      Json::Reader reader;
      if (!reader.parse(req.body, body) || !body.isObject()) {
        return make_err(request_id, 400, "PARAM_JSON", "body must be a JSON object");
      }
    }
    if (parts.size() == 3 && parts[0] == "streams") {
      if (parts[2] == "infer-interval") {
        return write_op(request_id, WriteOp::kSetInferInterval, parts[1], body);
      }
      if (parts[2] == "priority") {
        return write_op(request_id, WriteOp::kSetPriority, parts[1], body);
      }
      if (parts[2] == "restart") {
        return write_op(request_id, WriteOp::kRestartStream, parts[1], body);
      }
      return make_err(request_id, 404, "PARAM_PATH",
                      "unknown stream action '" + parts[2] + "'");
    }
    if (parts == std::vector<std::string>{"config", "snapshot"}) {
      return handle_snapshot(request_id, body);
    }
    if (parts == std::vector<std::string>{"config", "rollback"}) {
      return handle_rollback(request_id, body);
    }
    return make_err(request_id, 404, "PARAM_PATH", "unknown path '" + req.path + "'");
  }

  return make_err(request_id, 405, "HTTP_METHOD",
                  "method '" + req.method + "' not allowed");
}

HttpResponse ControlServer::make_ok(const std::string& request_id,
                                    const Json::Value& data,
                                    const std::string& snapshot_id) {
  Json::Value envelope;
  envelope["success"] = true;
  envelope["request_id"] = request_id;
  envelope["timestamp_ms"] = Json::Value::UInt64(wall_ms());
  envelope["data"] = data;
  if (!snapshot_id.empty()) {
    envelope["snapshot_id"] = snapshot_id;
  }
  HttpResponse resp;
  resp.status = 200;
  resp.body = json_str(envelope);
  return resp;
}

HttpResponse ControlServer::make_err(const std::string& request_id,
                                     int http_status, const char* error_code,
                                     const std::string& message) {
  Json::Value envelope;
  envelope["success"] = false;
  envelope["request_id"] = request_id;
  envelope["timestamp_ms"] = Json::Value::UInt64(wall_ms());
  envelope["error_code"] = error_code;
  envelope["error"] = message;
  HttpResponse resp;
  resp.status = http_status;
  resp.body = json_str(envelope);
  return resp;
}

// ---- read handlers ----------------------------------------------------------

Json::Value ControlServer::json_health() const {
  Json::Value data;
  data["status"] = "ok";
  data["streams"] = static_cast<int>(backend_->stream_ids().size());
  return data;
}

Json::Value ControlServer::json_metrics_summary() const {
  Json::Value data;
  Json::Value streams(Json::arrayValue);
  for (const auto& s : backend_->metrics_summary()) {
    Json::Value js;
    js["stream_id"] = s.stream_id;
    js["input_frames"] = Json::Value::UInt64(s.input_frames);
    js["infer_frames"] = Json::Value::UInt64(s.infer_frames);
    js["output_frames"] = Json::Value::UInt64(s.output_frames);
    js["detections"] = Json::Value::UInt64(s.total_detections);
    js["obj_per_frame"] = s.avg_detections_per_frame;
    js["input_fps"] = s.avg_input_fps;
    js["infer_fps"] = s.avg_infer_fps;
    js["output_fps"] = s.avg_output_fps;
    js["latest_input_fps"] = s.latest_input_fps;
    js["latest_infer_fps"] = s.latest_infer_fps;
    js["latest_output_fps"] = s.latest_output_fps;
    streams.append(js);
  }
  data["streams"] = streams;
  return data;
}

Json::Value ControlServer::json_stream_status(const std::string& stream_id,
                                              bool* found) const {
  const auto statuses = backend_->stream_status();
  Json::Value streams(Json::arrayValue);
  Json::Value single;
  for (const auto& st : statuses) {
    if (!stream_id.empty() && st.stream_id != stream_id) {
      continue;
    }
    Json::Value js;
    js["stream_id"] = st.stream_id;
    js["type"] = st.type;
    js["state"] = st.state;
    js["priority"] = st.priority;
    js["infer_interval"] = st.infer_interval;
    js["frames"] = Json::Value::UInt64(st.frames);
    js["reconnect_count"] = st.reconnect_count;
    js["consecutive_failures"] = st.consecutive_failures;
    js["last_reason"] = st.last_reason;
    if (stream_id.empty()) {
      streams.append(js);
    } else {
      single = js;
      if (found) *found = true;
    }
  }
  if (!stream_id.empty()) {
    if (found && !*found) {
      return Json::Value(Json::objectValue);
    }
    return single;
  }
  Json::Value data;
  data["streams"] = streams;
  return data;
}

Json::Value ControlServer::json_scheduler_config() const {
  const auto c = backend_->scheduler_config();
  Json::Value data;
  data["enable"] = c.enable;
  data["sample_interval_sec"] = c.sample_interval_sec;
  data["pressure_cpu_enter"] = c.pressure_cpu_enter;
  data["pressure_cpu_exit"] = c.pressure_cpu_exit;
  data["thermal_temp_enter"] = c.thermal_temp_enter;
  data["thermal_temp_exit"] = c.thermal_temp_exit;
  data["critical_temp_enter"] = c.critical_temp_enter;
  data["critical_temp_exit"] = c.critical_temp_exit;
  data["min_hold_ms"] = Json::Value::UInt64(c.min_hold_ms);
  data["cooldown_ms"] = Json::Value::UInt64(c.cooldown_ms);
  data["max_adjustments_per_window"] = c.max_adjustments_per_window;
  data["adjust_window_ms"] = Json::Value::UInt64(c.adjust_window_ms);
  return data;
}

Json::Value ControlServer::json_scheduler_state() const {
  const auto st = backend_->scheduler_status();
  Json::Value data;
  data["enabled"] = st.enabled;
  data["state"] = st.state;
  data["table_high"] = st.table_high;
  data["table_normal"] = st.table_normal;
  data["table_low"] = st.table_low;
  data["cpu_pct"] = st.cpu_pct;
  data["mem_pct"] = st.mem_pct;
  data["temp_c"] = st.temp_c;
  data["adjustments"] = st.adjustments;
  data["max_adjustments"] = st.max_adjustments;
  data["recovery_step"] = st.recovery_step;
  return data;
}

Json::Value ControlServer::json_recent_errors() const {
  const auto errors = backend_->recent_errors(64);
  Json::Value arr(Json::arrayValue);
  for (const auto& e : errors) {
    Json::Value je;
    je["ts_ms"] = Json::Value::UInt64(e.ts_ms);
    je["level"] = e.level;
    je["stream_id"] = e.stream_id;
    je["state"] = e.state;
    je["operation"] = e.operation;
    je["error_code"] = e.error_code;
    je["message"] = e.message;
    arr.append(je);
  }
  Json::Value data;
  data["errors"] = arr;
  data["count"] = static_cast<int>(arr.size());
  return data;
}

// ---- write ops (CLAUDE.md §16 flow) -----------------------------------------

bool ControlServer::capture_and_save_snapshot(const std::string& reason,
                                              std::string* snapshot_id) {
  ConfigSnapshot snap = backend_->current_config_snapshot();
  snap.snapshot_id = next_snapshot_id();
  snap.created_at_ms = wall_ms();
  snap.reason = reason;
  if (!snapshots_.save(snap)) {
    LOG_ERROR("control", "", "snapshot", "SNAP013", "pre-change snapshot failed: %s",
              reason.c_str());
    return false;
  }
  *snapshot_id = snap.snapshot_id;
  return true;
}

Json::Value ControlServer::stream_state_json(const std::vector<StreamStatus>& statuses,
                                             int idx) const {
  Json::Value js;
  if (idx >= 0 && idx < static_cast<int>(statuses.size())) {
    const auto& st = statuses[idx];
    js["stream_id"] = st.stream_id;
    js["priority"] = st.priority;
    js["infer_interval"] = st.infer_interval;
    js["state"] = st.state;
  } else {
    js["stream_id"] = "";
    js["priority"] = "";
    js["infer_interval"] = -1;
    js["state"] = "";
  }
  return js;
}

void ControlServer::audit(const std::string& request_id, const std::string& operation,
                          const std::string& stream_id, const Json::Value& args,
                          const Json::Value& before, const Json::Value& after,
                          bool success, const std::string& error_code,
                          const std::string& snapshot_id) {
  AuditRecord r;
  r.ts_ms = wall_ms();
  r.request_id = request_id;
  r.operation = operation;
  r.stream_id = stream_id;
  r.args = json_str(args);
  r.before = json_str(before);
  r.after = json_str(after);
  r.success = success;
  r.error_code = error_code;
  r.snapshot_id = snapshot_id;
  if (!audit_.append(r)) {
    LOG_ERROR("control", stream_id.c_str(), "audit", "AUDIT012",
              "audit append failed for request %s", request_id.c_str());
  }
}

HttpResponse ControlServer::write_op(const std::string& request_id, WriteOp op,
                                     const std::string& stream_id,
                                     const Json::Value& body) {
  std::lock_guard<std::mutex> lock(write_mu_);

  // 1. Parameter validation (types + ranges).
  int interval = -1;
  pipeline::StreamPriority priority = pipeline::StreamPriority::kNormal;
  std::string priority_str_val;
  switch (op) {
    case WriteOp::kSetInferInterval:
      if (!body.isMember("interval") || !body["interval"].isInt()) {
        return make_err(request_id, 400, "PARAM_INTERVAL",
                        "'interval' (int) is required");
      }
      interval = body["interval"].asInt();
      if (!valid_infer_interval(interval, cfg_.max_infer_interval)) {
        return make_err(request_id, 400, "PARAM_INTERVAL",
                        "interval must be in [0," +
                            std::to_string(cfg_.max_infer_interval) + "]");
      }
      break;
    case WriteOp::kSetPriority:
      if (!body.isMember("priority") || !body["priority"].isString()) {
        return make_err(request_id, 400, "PARAM_PRIORITY",
                        "'priority' (string) is required");
      }
      priority_str_val = body["priority"].asString();
      if (!valid_priority(priority_str_val)) {
        return make_err(request_id, 400, "PARAM_PRIORITY",
                        "priority must be 'high', 'normal' or 'low'");
      }
      priority = pipeline::priority_from_str(priority_str_val);
      break;
    case WriteOp::kRestartStream:
      break;
    case WriteOp::kRollback:
      return make_err(request_id, 400, "PARAM_PATH",
                      "rollback is handled by POST /config/rollback");
  }

  // 2. Stream lookup.
  const auto ids = backend_->stream_ids();
  const int idx = find_stream_index(ids, stream_id);
  if (idx < 0) {
    return make_err(request_id, 404, "PARAM_STREAM", "unknown stream '" + stream_id + "'");
  }

  // Per-stream restart throttle (implementation_plan §59): at most one
  // restart per stream per restart_min_interval_ms.  Recorded at request
  // time so a rejected restart cannot be hammered.
  if (op == WriteOp::kRestartStream) {
    if (last_restart_ms_.size() <= static_cast<size_t>(idx)) {
      last_restart_ms_.resize(idx + 1, 0);
    }
    const uint64_t now = mono_ms();
    if (last_restart_ms_[idx] != 0 &&
        now - last_restart_ms_[idx] <
            static_cast<uint64_t>(cfg_.restart_min_interval_ms)) {
      return make_err(request_id, 409, "RESTART_THROTTLED",
                      "restart for stream '" + stream_id +
                          "' was requested less than " +
                          std::to_string(cfg_.restart_min_interval_ms) + " ms ago");
    }
    last_restart_ms_[idx] = now;
  }

  // 3. Safety state check (CRITICAL blocks load increases).
  const auto statuses = backend_->stream_status();
  if (idx >= static_cast<int>(statuses.size())) {
    return make_err(request_id, 500, "INTERNAL", "stream status out of range");
  }
  const auto& before = statuses[idx];
  bool load_increase = false;
  switch (op) {
    case WriteOp::kSetInferInterval:
      load_increase = interval < before.infer_interval;
      break;
    case WriteOp::kSetPriority:
      load_increase = priority_ranks_up(
          pipeline::priority_from_str(before.priority), priority);
      break;
    default:
      break;  // restart / rollback never increase load by themselves
  }
  if (load_increase && !backend_->safety_state_allows()) {
    return make_err(request_id, 409, "SAFETY_CRITICAL",
                    "scheduler is CRITICAL — load-increasing changes are blocked");
  }

  // 4. Pre-change snapshot.
  std::string snapshot_id;
  if (!capture_and_save_snapshot("before " + std::to_string(static_cast<int>(op)) +
                                     " " + stream_id,
                                 &snapshot_id)) {
    return make_err(request_id, 500, "SNAPSHOT_IO", "pre-change snapshot failed");
  }

  // 5. Apply the bounded change.
  WriteResult result;
  switch (op) {
    case WriteOp::kSetInferInterval:
      result = backend_->set_infer_interval(idx, interval);
      break;
    case WriteOp::kSetPriority:
      result = backend_->set_priority(idx, priority);
      break;
    case WriteOp::kRestartStream:
      result = backend_->restart_stream(idx);
      break;
    default:
      result = WriteResult{false, "INTERNAL", "unreachable"};
      break;
  }

  // 6. Audit the apply outcome.
  const auto after_statuses = backend_->stream_status();
  const auto args_json = [&]() {
    Json::Value a(Json::objectValue);
    switch (op) {
      case WriteOp::kSetInferInterval: a["interval"] = interval; break;
      case WriteOp::kSetPriority: a["priority"] = priority_str_val; break;
      default: break;
    }
    return a;
  }();
  const std::string op_name = [&]() {
    switch (op) {
      case WriteOp::kSetInferInterval: return std::string("set_infer_interval");
      case WriteOp::kSetPriority: return std::string("set_priority");
      case WriteOp::kRestartStream: return std::string("restart_stream");
      case WriteOp::kRollback: return std::string("rollback");
    }
    return std::string("?");
  }();
  audit(request_id, op_name, stream_id, args_json,
        stream_state_json(statuses, idx), stream_state_json(after_statuses, idx),
        result.success, result.error_code, snapshot_id);

  if (!result.success) {
    // 7. Automatic rollback on apply failure: restore the pre-change
    // snapshot captured in step 4 (never the current, half-applied state).
    ConfigSnapshot pre;
    const bool have_pre = snapshots_.load(snapshot_id, &pre);
    const WriteResult rb = have_pre ? backend_->apply_snapshot(pre)
                                    : WriteResult{false, "SNAPSHOT_IO", "pre-change snapshot lost"};
    audit(request_id, "auto_rollback", stream_id, Json::Value(Json::objectValue),
          stream_state_json(after_statuses, idx),
          stream_state_json(backend_->stream_status(), idx),
          rb.success, rb.error_code, snapshot_id);
    return make_err(request_id, 409, result.error_code.c_str(), result.detail);
  }

  // 8. Read-back verification.
  bool verified = true;
  std::string verify_note;
  if (op == WriteOp::kSetInferInterval) {
    const int idx_now = find_stream_index(backend_->stream_ids(), stream_id);
    const auto st = backend_->stream_status();
    verified = idx_now >= 0 && idx_now < static_cast<int>(st.size()) &&
               st[idx_now].infer_interval == interval;
    if (!verified) {
      verify_note = "read-back interval mismatch";
    }
  } else if (op == WriteOp::kSetPriority) {
    const int idx_now = find_stream_index(backend_->stream_ids(), stream_id);
    const auto st = backend_->stream_status();
    verified = idx_now >= 0 && idx_now < static_cast<int>(st.size()) &&
               st[idx_now].priority == priority_str_val;
    if (!verified) {
      verify_note = "read-back priority mismatch";
    }
  } else {  // restart: the stream must be entering a reconnect state
    const int idx_now = find_stream_index(backend_->stream_ids(), stream_id);
    const auto st = backend_->stream_status();
    verified = idx_now >= 0 && idx_now < static_cast<int>(st.size()) &&
               (st[idx_now].state == "RECONNECTING" || st[idx_now].state == "CONNECTING");
    if (!verified) {
      verify_note = "stream did not enter RECONNECTING/CONNECTING";
    }
  }

  if (!verified) {
    ConfigSnapshot pre;
    const bool have_pre = snapshots_.load(snapshot_id, &pre);
    const WriteResult rb = have_pre ? backend_->apply_snapshot(pre)
                                    : WriteResult{false, "SNAPSHOT_IO", "pre-change snapshot lost"};
    audit(request_id, "auto_rollback", stream_id, Json::Value(Json::objectValue),
          stream_state_json(after_statuses, idx),
          stream_state_json(backend_->stream_status(), idx),
          rb.success, rb.error_code, snapshot_id);
    return make_err(request_id, 500, "VERIFY_FAILED", verify_note);
  }

  const Json::Value data = json_stream_status(stream_id, nullptr);
  return make_ok(request_id, data, snapshot_id);
}

HttpResponse ControlServer::handle_snapshot(const std::string& request_id,
                                            const Json::Value& body) {
  std::string reason = "explicit snapshot";
  if (body.isMember("reason")) {
    if (!body["reason"].isString()) {
      return make_err(request_id, 400, "PARAM_JSON", "'reason' must be a string");
    }
    reason = body["reason"].asString();
  }
  std::string snapshot_id;
  if (!capture_and_save_snapshot(reason, &snapshot_id)) {
    return make_err(request_id, 500, "SNAPSHOT_IO", "snapshot save failed");
  }
  Json::Value args;
  args["reason"] = reason;
  audit(request_id, "config_snapshot", "", args, Json::Value(Json::objectValue),
        Json::Value(Json::objectValue), true, "", snapshot_id);

  Json::Value data;
  data["snapshot_id"] = snapshot_id;
  return make_ok(request_id, data, snapshot_id);
}

HttpResponse ControlServer::handle_rollback(const std::string& request_id,
                                            const Json::Value& body) {
  if (!body.isMember("snapshot_id") || !body["snapshot_id"].isString()) {
    return make_err(request_id, 400, "PARAM_JSON", "'snapshot_id' (string) is required");
  }
  const std::string snapshot_id = body["snapshot_id"].asString();

  std::lock_guard<std::mutex> lock(write_mu_);

  ConfigSnapshot target;
  if (!snapshots_.load(snapshot_id, &target)) {
    return make_err(request_id, 404, "PARAM_SNAPSHOT",
                    "unknown snapshot '" + snapshot_id + "'");
  }

  // Pre-rollback snapshot (rollback is itself a write op: reversible).
  std::string pre_id;
  if (!capture_and_save_snapshot("before rollback " + snapshot_id, &pre_id)) {
    return make_err(request_id, 500, "SNAPSHOT_IO", "pre-rollback snapshot failed");
  }

  const WriteResult result = backend_->apply_snapshot(target);

  Json::Value args;
  args["snapshot_id"] = snapshot_id;
  audit(request_id, "rollback", "", args, Json::Value(Json::objectValue),
        Json::Value(Json::objectValue), result.success, result.error_code, pre_id);

  if (!result.success) {
    return make_err(request_id, 409, result.error_code.c_str(), result.detail);
  }

  // Verify every restored field matches the snapshot.
  bool verified = true;
  std::string verify_note;
  for (const auto& entry : target.streams) {
    const int idx = find_stream_index(backend_->stream_ids(), entry.stream_id);
    if (idx < 0) {
      verified = false;
      verify_note = "stream '" + entry.stream_id + "' no longer exists";
      break;
    }
    const auto st = backend_->stream_status();
    if (idx >= static_cast<int>(st.size())) {
      verified = false;
      verify_note = "stream status out of range";
      break;
    }
    if (st[idx].priority != pipeline::priority_str(entry.priority) ||
        st[idx].infer_interval != entry.infer_interval) {
      verified = false;
      verify_note = "stream '" + entry.stream_id + "' did not restore to snapshot";
      break;
    }
  }

  if (!verified) {
    audit(request_id, "rollback_verify_failed", "", Json::Value(Json::objectValue),
          Json::Value(Json::objectValue), Json::Value(Json::objectValue), false,
          "VERIFY_FAILED", pre_id);
    return make_err(request_id, 500, "VERIFY_FAILED", verify_note);
  }

  Json::Value data;
  data["snapshot_id"] = snapshot_id;
  return make_ok(request_id, data, snapshot_id);
}

std::string ControlServer::next_request_id() {
  return "req_" + std::to_string(wall_ms()) + "_" + std::to_string(seq_++);
}

std::string ControlServer::next_snapshot_id() {
  return "snap_" + std::to_string(wall_ms()) + "_" + std::to_string(seq_++);
}

}  // namespace control
}  // namespace jetedge
