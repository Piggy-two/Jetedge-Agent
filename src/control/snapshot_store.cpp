#include "jetedge/control/snapshot_store.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <json/json.h>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace control {

namespace {

std::string priority_to_string(pipeline::StreamPriority p) {
  return pipeline::priority_str(p);
}

bool priority_from_string(const std::string& s, pipeline::StreamPriority* out) {
  if (s == "high") { *out = pipeline::StreamPriority::kHigh; return true; }
  if (s == "normal") { *out = pipeline::StreamPriority::kNormal; return true; }
  if (s == "low") { *out = pipeline::StreamPriority::kLow; return true; }
  return false;
}

}  // namespace

bool SnapshotStore::init(const std::string& dir, int max_snapshots) {
  dir_ = dir;
  max_snapshots_ = max_snapshots > 0 ? max_snapshots : 1;
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  if (ec) {
    LOG_ERROR("control", "", "init", "SNAP010", "cannot create state dir '%s': %s",
              dir.c_str(), ec.message().c_str());
    return false;
  }
  return true;
}

bool SnapshotStore::save(const ConfigSnapshot& snap) {
  if (dir_.empty()) {
    return false;
  }

  Json::Value root;
  root["snapshot_id"] = snap.snapshot_id;
  root["created_at_ms"] = Json::Value::UInt64(snap.created_at_ms);
  root["reason"] = snap.reason;
  root["scheduler_enabled"] = snap.scheduler_enabled;
  Json::Value streams(Json::arrayValue);
  for (const auto& s : snap.streams) {
    Json::Value js;
    js["stream_id"] = s.stream_id;
    js["priority"] = priority_to_string(s.priority);
    js["infer_interval"] = s.infer_interval;
    streams.append(js);
  }
  root["streams"] = streams;

  Json::FastWriter writer;
  const std::string path = file_path(snap.snapshot_id);
  std::ofstream ofs(path, std::ios::trunc);
  if (!ofs) {
    LOG_ERROR("control", "", "save", "SNAP011", "cannot write snapshot '%s'",
              path.c_str());
    return false;
  }
  ofs << writer.write(root);
  ofs.close();
  if (!ofs) {
    LOG_ERROR("control", "", "save", "SNAP011", "write failed for snapshot '%s'",
              path.c_str());
    return false;
  }

  // Prune oldest beyond max_snapshots_ (newest first; keep the head).
  const auto ids = list();
  if (static_cast<int>(ids.size()) > max_snapshots_) {
    for (size_t i = max_snapshots_; i < ids.size(); ++i) {
      std::error_code ec;
      std::filesystem::remove(file_path(ids[i]), ec);
    }
  }
  return true;
}

bool SnapshotStore::load(const std::string& snapshot_id, ConfigSnapshot* out) const {
  std::string content;
  if (!read_file(file_path(snapshot_id), &content)) {
    return false;
  }

  Json::Value root;
  Json::Reader reader;
  if (!reader.parse(content, root) || !root.isObject()) {
    LOG_ERROR("control", "", "load", "SNAP012", "malformed snapshot '%s'",
              snapshot_id.c_str());
    return false;
  }

  ConfigSnapshot snap;
  snap.snapshot_id = root.get("snapshot_id", snapshot_id).asString();
  snap.created_at_ms = root.get("created_at_ms", Json::Value::UInt64(0)).asUInt64();
  snap.reason = root.get("reason", "").asString();
  snap.scheduler_enabled = root.get("scheduler_enabled", false).asBool();
  const Json::Value& streams = root["streams"];
  if (!streams.isArray()) {
    return false;
  }
  for (const auto& js : streams) {
    StreamSnapshotEntry e;
    e.stream_id = js.get("stream_id", "").asString();
    if (e.stream_id.empty() ||
        !priority_from_string(js.get("priority", "").asString(), &e.priority) ||
        !js["infer_interval"].isInt()) {
      LOG_ERROR("control", "", "load", "SNAP012", "malformed stream entry in '%s'",
                snapshot_id.c_str());
      return false;
    }
    e.infer_interval = js["infer_interval"].asInt();
    snap.streams.push_back(std::move(e));
  }

  *out = std::move(snap);
  return true;
}

std::vector<std::string> SnapshotStore::list() const {
  std::vector<std::string> ids;
  std::error_code ec;
  if (dir_.empty() || !std::filesystem::is_directory(dir_, ec)) {
    return ids;
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
      continue;
    }
    ids.push_back(entry.path().stem().string());
  }
  std::sort(ids.rbegin(), ids.rend());  // newest-first by id ordering
  return ids;
}

std::string SnapshotStore::file_path(const std::string& snapshot_id) const {
  return dir_ + "/" + snapshot_id + ".json";
}

bool SnapshotStore::read_file(const std::string& path, std::string* out) const {
  std::ifstream ifs(path);
  if (!ifs) {
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
  if (!ifs.eof() && ifs.fail()) {
    return false;
  }
  *out = std::move(content);
  return true;
}

}  // namespace control
}  // namespace jetedge
