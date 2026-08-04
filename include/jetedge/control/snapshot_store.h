// SnapshotStore — config snapshots as JSON files on disk (Stage 11).
//
// Every write op saves a pre-change snapshot here before applying; the
// rollback endpoint restores one.  Files live under the control state dir
// (runtime data, never committed).  Bounded: at most `max_snapshots` files
// are kept, oldest pruned on save.

#pragma once

#include <string>
#include <vector>

#include "jetedge/control/control_backend.h"

namespace jetedge {
namespace control {

class SnapshotStore {
 public:
  // Create the state dir if missing.  Returns false when the dir is unusable.
  bool init(const std::string& dir, int max_snapshots);

  // Serialize + write `snap` (snapshot_id must already be set).  Returns
  // false on I/O failure.  Prunes oldest files beyond max_snapshots.
  bool save(const ConfigSnapshot& snap);

  // Load one snapshot by id.  Returns false when missing or malformed.
  bool load(const std::string& snapshot_id, ConfigSnapshot* out) const;

  // All snapshot ids, newest first.
  std::vector<std::string> list() const;

  const std::string& dir() const { return dir_; }
  int max_snapshots() const { return max_snapshots_; }

 private:
  std::string file_path(const std::string& snapshot_id) const;
  bool read_file(const std::string& path, std::string* out) const;

  std::string dir_;
  int max_snapshots_ = 32;
};

}  // namespace control
}  // namespace jetedge
