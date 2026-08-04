#include "jetedge/control/error_store.h"

#include <algorithm>

namespace jetedge {
namespace control {

ErrorStore::ErrorStore(size_t capacity) : capacity_(capacity > 0 ? capacity : 1) {}

void ErrorStore::add(ErrorRecord r) {
  std::lock_guard<std::mutex> lock(mu_);
  records_.push_back(std::move(r));
  if (records_.size() > capacity_) {
    // Overwrite the oldest.
    records_.erase(records_.begin());
  }
}

void ErrorStore::clear() {
  std::lock_guard<std::mutex> lock(mu_);
  records_.clear();
}

std::vector<ErrorRecord> ErrorStore::recent(size_t limit) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<ErrorRecord> out;
  out.reserve(limit == 0 ? records_.size() : std::min(limit, records_.size()));
  for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
    if (limit != 0 && out.size() >= limit) {
      break;
    }
    out.push_back(*it);
  }
  return out;
}

size_t ErrorStore::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return records_.size();
}

}  // namespace control
}  // namespace jetedge
