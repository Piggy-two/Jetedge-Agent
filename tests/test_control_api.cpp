// Unit tests for the Stage 11 safe Control API.
// Pure logic + a FakeBackend — no GStreamer dependency:
//   - parameter validation (param_validation)
//   - bounded error store
//   - snapshot save/load/prune round-trip
//   - audit log JSONL records
//   - HTTP request parsing helpers
//   - the full CLAUDE.md §16 write-op flow (validate → safety → snapshot →
//     apply → audit → verify → auto-rollback) via ControlServer + FakeBackend

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <json/json.h>

#include "jetedge/control/audit_log.h"
#include "jetedge/control/control_server.h"
#include "jetedge/control/error_store.h"
#include "jetedge/control/http_server.h"
#include "jetedge/control/param_validation.h"
#include "jetedge/control/snapshot_store.h"

using jetedge::control::AuditLog;
using jetedge::control::AuditRecord;
using jetedge::control::ConfigSnapshot;
using jetedge::control::ControlBackend;
using jetedge::control::ControlConfig;
using jetedge::control::ControlServer;
using jetedge::control::ErrorRecord;
using jetedge::control::ErrorStore;
using jetedge::control::HttpRequest;
using jetedge::control::HttpResponse;
using jetedge::control::HttpServer;
using jetedge::control::SchedulerStatus;
using jetedge::control::SnapshotStore;
using jetedge::control::StreamSnapshotEntry;
using jetedge::control::StreamStatus;
using jetedge::control::WriteResult;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);               \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

#define CHECK_EQ(a, b)                                                  \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!((a) == (b))) {                                                \
      std::printf("FAIL line %d: %s == %s (%s vs %s)\n", __LINE__,      \
                  #a, #b, (std::ostringstream() << (a)).str().c_str(),  \
                  (std::ostringstream() << (b)).str().c_str());         \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

namespace {

// ---- FakeBackend: in-memory ControlBackend ----------------------------------

class FakeBackend : public ControlBackend {
 public:
  struct Stream {
    std::string id;
    std::string type = "file";
    std::string priority = "normal";
    int interval = 0;
    std::string state = "RUNNING";
    bool rtsp = false;
    bool failed = false;
  };
  std::vector<Stream> streams;
  bool critical = false;
  int set_interval_calls = 0;
  int restart_calls = 0;
  int rollback_calls = 0;
  bool fail_apply = false;       // set_infer_interval returns an error
  bool verify_mismatch = false;  // applied value never sticks (read-back differs)

  std::vector<std::string> stream_ids() const override {
    std::vector<std::string> out;
    for (const auto& s : streams) out.push_back(s.id);
    return out;
  }

  std::vector<StreamStatus> stream_status() const override {
    std::vector<StreamStatus> out;
    for (const auto& s : streams) {
      StreamStatus st;
      st.stream_id = s.id;
      st.type = s.type;
      st.state = s.state;
      st.priority = s.priority;
      st.infer_interval = s.interval;
      out.push_back(st);
    }
    return out;
  }

  std::vector<jetedge::metrics::MetricsRegistry::StreamSummary> metrics_summary() const override {
    return {};
  }

  SchedulerStatus scheduler_status() const override {
    SchedulerStatus st;
    st.enabled = true;
    st.state = critical ? "CRITICAL" : "NORMAL";
    return st;
  }

  jetedge::scheduler::SchedulerConfig scheduler_config() const override { return {}; }

  std::vector<ErrorRecord> recent_errors(size_t) const override { return {}; }

  bool safety_state_allows() const override { return !critical; }

  WriteResult set_infer_interval(int idx, int interval) override {
    ++set_interval_calls;
    if (fail_apply) {
      return {false, "APPLY_FAILED", "injected apply failure"};
    }
    if (verify_mismatch) {
      return {true, "", ""};  // applied but read-back will show the old value
    }
    streams[idx].interval = interval;
    return {true, "", ""};
  }

  WriteResult set_priority(int idx, jetedge::pipeline::StreamPriority p) override {
    streams[idx].priority = jetedge::pipeline::priority_str(p);
    return {true, "", ""};
  }

  WriteResult restart_stream(int idx) override {
    ++restart_calls;
    if (!streams[idx].rtsp) {
      return {false, "RTSP_ONLY", "restart is only supported for RTSP streams"};
    }
    if (streams[idx].failed) {
      return {false, "RTSP_FAILED", "stream is FAILED"};
    }
    streams[idx].state = "RECONNECTING";
    return {true, "", ""};
  }

  ConfigSnapshot current_config_snapshot() const override {
    ConfigSnapshot snap;
    snap.scheduler_enabled = true;
    for (const auto& s : streams) {
      StreamSnapshotEntry e;
      e.stream_id = s.id;
      e.priority = jetedge::pipeline::priority_from_str(s.priority);
      e.infer_interval = s.interval;
      snap.streams.push_back(e);
    }
    return snap;
  }

  WriteResult apply_snapshot(const ConfigSnapshot& snap) override {
    ++rollback_calls;
    for (const auto& e : snap.streams) {
      for (auto& s : streams) {
        if (s.id == e.stream_id) {
          s.priority = jetedge::pipeline::priority_str(e.priority);
          s.interval = e.infer_interval;
        }
      }
    }
    return {true, "", ""};
  }
};

// Build a ControlServer wired to a FakeBackend with the given streams.
// state_dir is a fresh temp dir (cleaned at the end of the test).
struct ServerFixture {
  std::filesystem::path dir;
  ControlConfig cfg;
  ControlServer server;
  FakeBackend backend;

  explicit ServerFixture(std::vector<FakeBackend::Stream> streams) {
    dir = std::filesystem::temp_directory_path() /
          ("jetedge_test_control_" + std::to_string(std::rand()));
    std::filesystem::remove_all(dir);
    cfg.enable = true;
    cfg.port = 0;  // ephemeral; tests call handle_request directly
    cfg.state_dir = dir.string();
    cfg.restart_min_interval_ms = 100000;  // throttle effectively off for tests
    backend.streams = std::move(streams);
    const bool ok = server.start(cfg, &backend);
    CHECK(ok);
  }

  ~ServerFixture() {
    server.stop();
    std::filesystem::remove_all(dir);
  }

  HttpResponse get(const std::string& path) {
    HttpRequest req;
    req.method = "GET";
    req.path = path;
    return server.handle_request(req);
  }

  HttpResponse post(const std::string& path, const std::string& body) {
    HttpRequest req;
    req.method = "POST";
    req.path = path;
    req.body = body;
    return server.handle_request(req);
  }

  bool response_success(const HttpResponse& r, const char* what) {
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(r.body, root)) {
      std::printf("FAIL(%s): response not JSON: %s\n", what, r.body.c_str());
      ++g_failures;
      return false;
    }
    if (!root.get("success", false).asBool()) {
      std::printf("FAIL(%s): success=false: %s\n", what, r.body.c_str());
      ++g_failures;
      return false;
    }
    ++g_checks;
    return true;
  }

  std::string response_error_code(const HttpResponse& r) {
    Json::Value root;
    Json::Reader reader;
    reader.parse(r.body, root);
    return root.get("error_code", "").asString();
  }

  std::string response_snapshot_id(const HttpResponse& r) {
    Json::Value root;
    Json::Reader reader;
    reader.parse(r.body, root);
    return root.get("snapshot_id", "").asString();
  }
};

FakeBackend::Stream mk_stream(const std::string& id, const std::string& priority) {
  FakeBackend::Stream s;
  s.id = id;
  s.priority = priority;
  return s;
}

// Read all non-empty lines of a file.
std::vector<std::string> read_lines(const std::string& path) {
  std::vector<std::string> out;
  std::ifstream ifs(path);
  std::string line;
  while (std::getline(ifs, line)) {
    if (!line.empty()) out.push_back(line);
  }
  return out;
}

}  // namespace

// ---- param_validation -------------------------------------------------------

static void test_param_validation() {
  CHECK(jetedge::control::valid_infer_interval(0, 5));
  CHECK(jetedge::control::valid_infer_interval(5, 5));
  CHECK(!jetedge::control::valid_infer_interval(-1, 5));
  CHECK(!jetedge::control::valid_infer_interval(6, 5));
  CHECK(!jetedge::control::valid_infer_interval(0, -1));

  CHECK(jetedge::control::valid_priority("high"));
  CHECK(jetedge::control::valid_priority("normal"));
  CHECK(jetedge::control::valid_priority("low"));
  CHECK(!jetedge::control::valid_priority("critical"));
  CHECK(!jetedge::control::valid_priority(""));
  CHECK(!jetedge::control::valid_priority("HIGH"));

  const std::vector<std::string> ids = {"cam1", "cam2", "cam3"};
  CHECK_EQ(jetedge::control::find_stream_index(ids, "cam1"), 0);
  CHECK_EQ(jetedge::control::find_stream_index(ids, "cam3"), 2);
  CHECK_EQ(jetedge::control::find_stream_index(ids, "cam4"), -1);
  CHECK_EQ(jetedge::control::find_stream_index({}, "cam1"), -1);

  using jetedge::pipeline::StreamPriority;
  CHECK(jetedge::control::priority_ranks_up(StreamPriority::kNormal, StreamPriority::kHigh));
  CHECK(jetedge::control::priority_ranks_up(StreamPriority::kLow, StreamPriority::kNormal));
  CHECK(!jetedge::control::priority_ranks_up(StreamPriority::kHigh, StreamPriority::kNormal));
  CHECK(!jetedge::control::priority_ranks_up(StreamPriority::kHigh, StreamPriority::kHigh));
}

// ---- error_store ------------------------------------------------------------

static void test_error_store() {
  ErrorStore store(4);
  for (int i = 0; i < 6; ++i) {
    ErrorRecord r;
    r.ts_ms = 1000 + i;
    r.level = "ERROR";
    r.stream_id = "cam1";
    r.error_code = "E" + std::to_string(i);
    r.message = "msg" + std::to_string(i);
    store.add(r);
  }
  CHECK_EQ(store.size(), 4u);
  const auto recent = store.recent(0);
  CHECK_EQ(recent.size(), 4u);
  CHECK_EQ(recent[0].message, "msg5");  // newest first
  CHECK_EQ(recent[3].message, "msg2");  // oldest kept
  const auto limited = store.recent(2);
  CHECK_EQ(limited.size(), 2u);
  CHECK_EQ(limited[0].message, "msg5");
  CHECK_EQ(limited[1].message, "msg4");

  store.clear();
  CHECK_EQ(store.size(), 0u);
  CHECK_EQ(store.recent(0).size(), 0u);
}

// ---- snapshot_store ---------------------------------------------------------

static void test_snapshot_store() {
  const auto dir =
      std::filesystem::temp_directory_path() /
      ("jetedge_test_snap_" + std::to_string(std::rand()));
  std::filesystem::remove_all(dir);

  SnapshotStore store;
  CHECK(store.init(dir.string(), 2));

  ConfigSnapshot s1;
  s1.snapshot_id = "snap_a";
  s1.created_at_ms = 111;
  s1.reason = "test";
  s1.scheduler_enabled = true;
  s1.streams = {
      {"cam1", jetedge::pipeline::StreamPriority::kHigh, 0},
      {"cam2", jetedge::pipeline::StreamPriority::kNormal, 2},
  };
  CHECK(store.save(s1));

  ConfigSnapshot loaded;
  CHECK(store.load("snap_a", &loaded));
  CHECK_EQ(loaded.snapshot_id, "snap_a");
  CHECK_EQ(loaded.created_at_ms, 111u);
  CHECK_EQ(loaded.reason, "test");
  CHECK(loaded.scheduler_enabled);
  CHECK_EQ(loaded.streams.size(), 2u);
  CHECK_EQ(loaded.streams[0].stream_id, "cam1");
  CHECK(loaded.streams[0].priority == jetedge::pipeline::StreamPriority::kHigh);
  CHECK_EQ(loaded.streams[0].infer_interval, 0);
  CHECK_EQ(loaded.streams[1].infer_interval, 2);

  // Missing snapshot.
  CHECK(!store.load("snap_missing", &loaded));
  // Malformed file → rejected, not crash.
  {
    std::ofstream ofs(dir / "snap_bad.json");
    ofs << "{ not json";
  }
  CHECK(!store.load("snap_bad", &loaded));

  // Prune: keep at most 2, oldest removed.
  ConfigSnapshot s3;
  s3.snapshot_id = "snap_c";
  s3.created_at_ms = 333;
  CHECK(store.save(s3));
  ConfigSnapshot s4;
  s4.snapshot_id = "snap_d";
  s4.created_at_ms = 444;
  CHECK(store.save(s4));
  const auto ids = store.list();
  CHECK_EQ(ids.size(), 2u);
  CHECK_EQ(ids[0], "snap_d");  // newest first
  CHECK_EQ(ids[1], "snap_c");

  std::filesystem::remove_all(dir);
}

// ---- audit_log --------------------------------------------------------------

static void test_audit_log() {
  const auto dir =
      std::filesystem::temp_directory_path() /
      ("jetedge_test_audit_" + std::to_string(std::rand()));
  std::filesystem::remove_all(dir);  // stale runs share the deterministic name
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "audit.jsonl").string();

  AuditLog log;
  CHECK(log.open(path));
  AuditRecord r;
  r.ts_ms = 42;
  r.request_id = "req_1";
  r.operation = "set_infer_interval";
  r.stream_id = "cam2";
  r.args = "{\"interval\":2}";
  r.before = "{\"infer_interval\":0}";
  r.after = "{\"infer_interval\":2}";
  r.success = true;
  r.error_code = "";
  r.snapshot_id = "snap_a";
  CHECK(log.append(r));
  log.close();

  const auto lines = read_lines(path);
  CHECK_EQ(lines.size(), 1u);
  Json::Value root;
  Json::Reader reader;
  CHECK(reader.parse(lines.back(), root));  // last line = this run's record
  CHECK_EQ(root["request_id"].asString(), "req_1");
  CHECK_EQ(root["operation"].asString(), "set_infer_interval");
  CHECK(root["success"].asBool());
  CHECK_EQ(root["snapshot_id"].asString(), "snap_a");
  CHECK(root["args"].isObject());
  CHECK_EQ(root["args"]["interval"].asInt(), 2);

  std::filesystem::remove_all(dir);
}

// ---- http parsing -----------------------------------------------------------

static void test_http_parse() {
  std::string method, path, query;

  CHECK(HttpServer::parse_request_line("GET /health HTTP/1.1", &method, &path, &query));
  CHECK_EQ(method, "GET");
  CHECK_EQ(path, "/health");
  CHECK_EQ(query, "");

  CHECK(HttpServer::parse_request_line("POST /streams/cam1/infer-interval?x=1 HTTP/1.1",
                                       &method, &path, &query));
  CHECK_EQ(method, "POST");
  CHECK_EQ(path, "/streams/cam1/infer-interval");
  CHECK_EQ(query, "x=1");

  CHECK(HttpServer::parse_request_line("GET /streams/cam%201 HTTP/1.1",
                                       &method, &path, &query));
  CHECK_EQ(path, "/streams/cam 1");  // percent-decoded

  CHECK(!HttpServer::parse_request_line("GET /health", &method, &path, &query));
  CHECK(!HttpServer::parse_request_line("GET /health HTTP/2.0", &method, &path, &query));
  CHECK(!HttpServer::parse_request_line("", &method, &path, &query));
  CHECK(!HttpServer::parse_request_line("GET relative HTTP/1.1", &method, &path, &query));

  std::map<std::string, std::string> headers;
  CHECK(HttpServer::parse_headers("Content-Length: 12\r\nContent-Type: application/json",
                                  &headers));
  CHECK_EQ(headers["content-length"], "12");  // lowercase key
  CHECK_EQ(headers["content-type"], "application/json");
  CHECK_EQ(HttpServer::parse_content_length(headers), 12);
  // Trailing blank line tolerated (callers may include the terminator).
  CHECK(HttpServer::parse_headers("Content-Length: 12\r\n\r\n", &headers));
  CHECK_EQ(headers["content-length"], "12");

  CHECK(!HttpServer::parse_headers("NoColonHere", &headers));  // clears the map
  std::map<std::string, std::string> empty;
  CHECK_EQ(HttpServer::parse_content_length(empty), 0);
  CHECK_EQ(HttpServer::parse_content_length({{"content-length", "-1"}}), -1);
  CHECK_EQ(HttpServer::parse_content_length({{"content-length", "12x"}}), -1);

  std::string decoded;
  CHECK(HttpServer::url_decode("a%20b+c", &decoded));
  CHECK_EQ(decoded, "a b c");
  CHECK(!HttpServer::url_decode("a%2", &decoded));
  CHECK(!HttpServer::url_decode("a%zz", &decoded));

  HttpRequest req;
  const std::string head =
      "POST /streams/cam1/infer-interval HTTP/1.1\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 15\r\n\r\n";  // 15 = byte length of "{\"interval\": 3}"
  CHECK(HttpServer::parse_request(head, "{\"interval\": 3}xx", 4096, &req));
  CHECK_EQ(req.method, "POST");
  CHECK_EQ(req.path, "/streams/cam1/infer-interval");
  CHECK_EQ(req.body, "{\"interval\": 3}");  // exactly 15 bytes, trailing "xx" dropped
  CHECK(!HttpServer::parse_request(head, "{\"interval\":", 4096, &req));  // truncated
  CHECK(!HttpServer::parse_request(head, "{\"interval\": 3}", 8, &req));  // body > cap
}

// ---- ControlServer: read endpoints ------------------------------------------

static void test_read_endpoints() {
  ServerFixture f({mk_stream("cam1", "high"), mk_stream("cam2", "normal")});

  CHECK(f.response_success(f.get("/health"), "health"));
  CHECK_EQ(f.get("/health").status, 200);

  const auto streams = f.get("/streams");
  CHECK(f.response_success(streams, "streams"));
  Json::Value root;
  Json::Reader reader;
  reader.parse(streams.body, root);
  CHECK_EQ(root["data"]["streams"].size(), 2u);
  CHECK_EQ(root["data"]["streams"][0]["stream_id"].asString(), "cam1");
  CHECK_EQ(root["data"]["streams"][0]["priority"].asString(), "high");

  const auto one = f.get("/streams/cam2");
  CHECK(f.response_success(one, "streams/cam2"));
  reader.parse(one.body, root);
  CHECK_EQ(root["data"]["stream_id"].asString(), "cam2");

  CHECK_EQ(f.get("/streams/nope").status, 404);
  CHECK_EQ(f.response_error_code(f.get("/streams/nope")), "PARAM_STREAM");

  CHECK(f.response_success(f.get("/metrics/summary"), "metrics"));
  CHECK(f.response_success(f.get("/scheduler/config"), "scheduler config"));
  CHECK(f.response_success(f.get("/scheduler/state"), "scheduler state"));
  CHECK(f.response_success(f.get("/errors/recent"), "errors"));

  CHECK_EQ(f.get("/unknown/path").status, 404);
  CHECK_EQ(f.response_error_code(f.get("/unknown/path")), "PARAM_PATH");

  HttpRequest del;
  del.method = "DELETE";
  del.path = "/health";
  const auto del_resp = f.server.handle_request(del);
  CHECK_EQ(del_resp.status, 405);
  CHECK_EQ(f.response_error_code(del_resp), "HTTP_METHOD");
}

// ---- ControlServer: write-op flow (CLAUDE.md §16) ---------------------------

static void test_write_flow_success() {
  ServerFixture f({mk_stream("cam1", "high"), mk_stream("cam2", "normal")});
  const int before_calls = f.backend.set_interval_calls;

  const auto resp = f.post("/streams/cam2/infer-interval", "{\"interval\": 2}");
  CHECK(f.response_success(resp, "set interval"));
  CHECK_EQ(resp.status, 200);
  CHECK_EQ(f.backend.set_interval_calls, before_calls + 1);
  CHECK_EQ(f.backend.streams[1].interval, 2);
  CHECK_EQ(f.backend.rollback_calls, 0);  // no rollback on success
  const std::string snap_id = f.response_snapshot_id(resp);
  CHECK(!snap_id.empty());

  // Pre-change snapshot was saved to disk.
  SnapshotStore store;
  CHECK(store.init(f.dir.string(), 32));
  ConfigSnapshot loaded;
  CHECK(store.load(snap_id, &loaded));
  CHECK_EQ(loaded.streams[1].infer_interval, 0);  // before value

  // Audit record exists with before/after.
  const auto lines = read_lines((f.dir / "audit.jsonl").string());
  CHECK_EQ(lines.size(), 1u);
  Json::Value root;
  Json::Reader reader;
  reader.parse(lines[0], root);
  CHECK_EQ(root["operation"].asString(), "set_infer_interval");
  CHECK_EQ(root["stream_id"].asString(), "cam2");
  CHECK_EQ(root["args"]["interval"].asInt(), 2);
  CHECK_EQ(root["snapshot_id"].asString(), snap_id);
  CHECK(root["success"].asBool());
}

static void test_write_flow_invalid_params() {
  ServerFixture f({mk_stream("cam1", "high")});
  const int before_calls = f.backend.set_interval_calls;

  // Out of range.
  auto resp = f.post("/streams/cam1/infer-interval", "{\"interval\": 6}");
  CHECK_EQ(resp.status, 400);
  CHECK_EQ(f.response_error_code(resp), "PARAM_INTERVAL");
  // Negative.
  resp = f.post("/streams/cam1/infer-interval", "{\"interval\": -1}");
  CHECK_EQ(f.response_error_code(resp), "PARAM_INTERVAL");
  // Wrong type.
  resp = f.post("/streams/cam1/infer-interval", "{\"interval\": \"2\"}");
  CHECK_EQ(f.response_error_code(resp), "PARAM_INTERVAL");
  // Missing field.
  resp = f.post("/streams/cam1/infer-interval", "{}");
  CHECK_EQ(f.response_error_code(resp), "PARAM_INTERVAL");
  // Malformed JSON body.
  resp = f.post("/streams/cam1/infer-interval", "not json");
  CHECK_EQ(resp.status, 400);
  CHECK_EQ(f.response_error_code(resp), "PARAM_JSON");
  // Unknown stream.
  resp = f.post("/streams/cam9/infer-interval", "{\"interval\": 1}");
  CHECK_EQ(resp.status, 404);
  CHECK_EQ(f.response_error_code(resp), "PARAM_STREAM");
  // Bad priority.
  resp = f.post("/streams/cam1/priority", "{\"priority\": \"critical\"}");
  CHECK_EQ(f.response_error_code(resp), "PARAM_PRIORITY");
  // Unknown action.
  resp = f.post("/streams/cam1/banish", "{}");
  CHECK_EQ(f.response_error_code(resp), "PARAM_PATH");

  // Nothing was applied, no snapshot, no audit.
  CHECK_EQ(f.backend.set_interval_calls, before_calls);
  CHECK_EQ(f.backend.streams[0].interval, 0);
  const auto lines = read_lines((f.dir / "audit.jsonl").string());
  CHECK_EQ(lines.size(), 0u);
}

static void test_write_flow_safety_gate() {
  ServerFixture f({mk_stream("cam1", "high"), mk_stream("cam4", "low")});
  f.backend.critical = true;

  // Load increase (interval 2 → 0) blocked in CRITICAL.
  f.backend.streams[1].interval = 2;
  auto resp = f.post("/streams/cam4/infer-interval", "{\"interval\": 0}");
  CHECK_EQ(resp.status, 409);
  CHECK_EQ(f.response_error_code(resp), "SAFETY_CRITICAL");
  CHECK_EQ(f.backend.streams[1].interval, 2);  // unchanged
  CHECK_EQ(f.backend.set_interval_calls, 0);   // not applied

  // Load decrease (interval 2 → 3) allowed in CRITICAL.
  resp = f.post("/streams/cam4/infer-interval", "{\"interval\": 3}");
  CHECK(f.response_success(resp, "interval increase allowed in CRITICAL"));
  CHECK_EQ(f.backend.streams[1].interval, 3);

  // Priority raise (low → high) blocked in CRITICAL.
  f.backend.streams[0].priority = "low";
  resp = f.post("/streams/cam1/priority", "{\"priority\": \"high\"}");
  CHECK_EQ(f.response_error_code(resp), "SAFETY_CRITICAL");
  CHECK_EQ(f.backend.streams[0].priority, "low");  // unchanged

  // Priority drop (high → low) allowed in CRITICAL.
  f.backend.streams[0].priority = "high";
  resp = f.post("/streams/cam1/priority", "{\"priority\": \"low\"}");
  CHECK(f.response_success(resp, "priority drop allowed in CRITICAL"));
  CHECK_EQ(f.backend.streams[0].priority, "low");
}

static void test_write_flow_apply_failure_rolls_back() {
  ServerFixture f({mk_stream("cam1", "high"), mk_stream("cam2", "normal")});
  f.backend.fail_apply = true;

  const auto resp = f.post("/streams/cam2/infer-interval", "{\"interval\": 2}");
  CHECK_EQ(resp.status, 409);
  CHECK_EQ(f.response_error_code(resp), "APPLY_FAILED");
  // Rollback was invoked to restore the pre-change state.
  CHECK(f.backend.rollback_calls >= 1);
  // Audit has the failed op + auto_rollback records.
  const auto lines = read_lines((f.dir / "audit.jsonl").string());
  CHECK_EQ(lines.size(), 2u);
  Json::Value root;
  Json::Reader reader;
  reader.parse(lines[1], root);
  CHECK_EQ(root["operation"].asString(), "auto_rollback");
}

static void test_write_flow_verify_failure_rolls_back() {
  ServerFixture f({mk_stream("cam1", "high"), mk_stream("cam2", "normal")});
  f.backend.verify_mismatch = true;  // apply reports success, value never sticks

  const auto resp = f.post("/streams/cam2/infer-interval", "{\"interval\": 2}");
  CHECK_EQ(resp.status, 500);
  CHECK_EQ(f.response_error_code(resp), "VERIFY_FAILED");
  CHECK(f.backend.rollback_calls >= 1);
}

static void test_restart_throttle() {
  ServerFixture f({mk_stream("cam1", "high"), mk_stream("cam2", "normal")});
  f.backend.streams[0].rtsp = true;

  // First restart: allowed.
  auto resp = f.post("/streams/cam1/restart", "{}");
  CHECK(f.response_success(resp, "first restart"));
  CHECK_EQ(f.backend.restart_calls, 1);

  // Second restart within restart_min_interval_ms: throttled.
  resp = f.post("/streams/cam1/restart", "{}");
  CHECK_EQ(resp.status, 409);
  CHECK_EQ(f.response_error_code(resp), "RESTART_THROTTLED");
  CHECK_EQ(f.backend.restart_calls, 1);  // backend not called

  // File stream: rejected by the backend.
  resp = f.post("/streams/cam2/restart", "{}");
  CHECK_EQ(resp.status, 409);
  CHECK_EQ(f.response_error_code(resp), "RTSP_ONLY");
}

static void test_snapshot_and_rollback_endpoints() {
  ServerFixture f({mk_stream("cam1", "high"), mk_stream("cam2", "normal")});

  // Explicit snapshot endpoint.
  auto resp = f.post("/config/snapshot", "{\"reason\": \"baseline\"}");
  CHECK(f.response_success(resp, "snapshot"));
  const std::string snap_id = f.response_snapshot_id(resp);
  CHECK(!snap_id.empty());

  // Mutate the state.
  resp = f.post("/streams/cam2/infer-interval", "{\"interval\": 3}");
  CHECK(f.response_success(resp, "set interval"));
  resp = f.post("/streams/cam1/priority", "{\"priority\": \"low\"}");
  CHECK(f.response_success(resp, "set priority"));
  CHECK_EQ(f.backend.streams[1].interval, 3);
  CHECK_EQ(f.backend.streams[0].priority, "low");

  // Rollback restores every field from the snapshot.
  resp = f.post("/config/rollback",
                "{\"snapshot_id\": \"" + snap_id + "\"}");
  CHECK(f.response_success(resp, "rollback"));
  CHECK_EQ(f.backend.streams[0].priority, "high");
  CHECK_EQ(f.backend.streams[1].interval, 0);
  CHECK_EQ(f.backend.streams[1].priority, "normal");

  // Unknown snapshot id.
  resp = f.post("/config/rollback", "{\"snapshot_id\": \"snap_missing\"}");
  CHECK_EQ(resp.status, 404);
  CHECK_EQ(f.response_error_code(resp), "PARAM_SNAPSHOT");

  // Missing snapshot_id field.
  resp = f.post("/config/rollback", "{}");
  CHECK_EQ(f.response_error_code(resp), "PARAM_JSON");
}

static void test_rollback_verify_restores() {
  // A snapshot created BEFORE a verify-mismatch world: rolling back must
  // still verify the restored values against the snapshot itself.
  ServerFixture f({mk_stream("cam1", "high")});
  auto resp = f.post("/config/snapshot", "{\"reason\": \"pre\"}");
  CHECK(f.response_success(resp, "snapshot"));
  const std::string snap_id = f.response_snapshot_id(resp);

  f.backend.verify_mismatch = true;
  resp = f.post("/streams/cam1/infer-interval", "{\"interval\": 4}");
  CHECK_EQ(f.response_error_code(resp), "VERIFY_FAILED");  // auto-rolled back

  f.backend.verify_mismatch = false;
  resp = f.post("/config/rollback", "{\"snapshot_id\": \"" + snap_id + "\"}");
  CHECK(f.response_success(resp, "rollback after mismatch"));
  CHECK_EQ(f.backend.streams[0].interval, 0);
}

int main() {
  test_param_validation();
  test_error_store();
  test_snapshot_store();
  test_audit_log();
  test_http_parse();
  test_read_endpoints();
  test_write_flow_success();
  test_write_flow_invalid_params();
  test_write_flow_safety_gate();
  test_write_flow_apply_failure_rolls_back();
  test_write_flow_verify_failure_rolls_back();
  test_restart_throttle();
  test_snapshot_and_rollback_endpoints();
  test_rollback_verify_restores();

  std::printf("test_control_api: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
