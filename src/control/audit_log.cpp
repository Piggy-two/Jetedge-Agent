#include "jetedge/control/audit_log.h"

#include <cstdio>
#include <cstring>

#include <json/json.h>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace control {

bool AuditLog::open(const std::string& path) {
  std::lock_guard<std::mutex> lock(mu_);
  path_ = path;
  fp_ = std::fopen(path.c_str(), "a");
  if (!fp_) {
    LOG_ERROR("control", "", "init", "AUDIT010", "cannot open audit log '%s': %s",
              path.c_str(), std::strerror(errno));
    return false;
  }
  return true;
}

bool AuditLog::append(const AuditRecord& r) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!fp_) {
    return false;
  }

  // args/before/after are JSON-encoded strings in the API; embed them as
  // real objects in the record so consumers (jq, the future Agent) can read
  // them directly.  Falls back to a plain string when not valid JSON.
  const auto embed = [](const std::string& s) -> Json::Value {
    if (s.empty()) {
      return Json::Value(Json::objectValue);
    }
    Json::Value v;
    Json::Reader reader;
    if (reader.parse(s, v) && v.isObject()) {
      return v;
    }
    return Json::Value(s);
  };

  Json::Value jr;
  jr["ts_ms"] = Json::Value::UInt64(r.ts_ms);
  jr["request_id"] = r.request_id;
  jr["operation"] = r.operation;
  jr["stream_id"] = r.stream_id;
  jr["args"] = embed(r.args);
  jr["before"] = embed(r.before);
  jr["after"] = embed(r.after);
  jr["success"] = r.success;
  jr["error_code"] = r.error_code;
  jr["snapshot_id"] = r.snapshot_id;

  Json::FastWriter writer;
  const std::string line = writer.write(jr);
  const size_t n = std::fwrite(line.c_str(), 1, line.size(),
                               static_cast<FILE*>(fp_));
  if (n != line.size() || std::fflush(static_cast<FILE*>(fp_)) != 0) {
    LOG_ERROR("control", "", "audit", "AUDIT011", "audit write failed: %s",
              std::strerror(errno));
    return false;
  }
  return true;
}

void AuditLog::close() {
  std::lock_guard<std::mutex> lock(mu_);
  if (fp_) {
    std::fclose(static_cast<FILE*>(fp_));
    fp_ = nullptr;
  }
}

}  // namespace control
}  // namespace jetedge
