// SystemSampler implementation (Stage 9).

#include "jetedge/scheduler/system_metrics.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace jetedge {
namespace scheduler {

namespace {

constexpr int kMaxThermalZones = 16;

}  // namespace

double SystemSampler::sample_cpu_pct() {
  std::ifstream in("/proc/stat");
  std::string line;
  if (!std::getline(in, line)) {
    return -1.0;
  }
  if (line.rfind("cpu ", 0) != 0) {
    return -1.0;
  }
  uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0,
           softirq = 0, steal = 0;
  std::istringstream ss(line.substr(4));
  ss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
  const uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
  const uint64_t busy = total - idle - iowait;

  if (!have_prev_) {
    have_prev_ = true;
    prev_busy_ = busy;
    prev_total_ = total;
    return 0.0;
  }
  const uint64_t dt = total - prev_total_;
  const uint64_t db = busy - prev_busy_;  // underflow guard below
  prev_busy_ = busy;
  prev_total_ = total;
  if (dt == 0) {
    return 0.0;
  }
  const uint64_t db_safe = std::min(db, dt);  // counter reset protection
  return static_cast<double>(db_safe) * 100.0 / static_cast<double>(dt);
}

double SystemSampler::sample_mem_pct() {
  std::ifstream in("/proc/meminfo");
  std::string key;
  uint64_t total = 0, available = 0;
  for (int i = 0; i < 64 && in; ++i) {
    in >> key;
    if (key == "MemTotal:") {
      in >> total;
    } else if (key == "MemAvailable:") {
      in >> available;
      break;
    } else {
      std::string skip;
      std::getline(in, skip);
    }
  }
  if (total == 0 || available > total) {
    return -1.0;
  }
  return static_cast<double>(total - available) * 100.0 / static_cast<double>(total);
}

double SystemSampler::sample_temp_c() {
  double max_temp = -1.0;
  std::string max_zone = "none";
  for (int i = 0; i < kMaxThermalZones; ++i) {
    const std::string path =
        "/sys/class/thermal/thermal_zone" + std::to_string(i) + "/temp";
    std::ifstream in(path);
    int milli_c = 0;
    if (!(in >> milli_c)) {
      continue;  // unreadable zone (absent on this device) — skip
    }
    const double temp_c = static_cast<double>(milli_c) / 1000.0;
    if (temp_c > max_temp) {
      max_temp = temp_c;
      std::ifstream type_in("/sys/class/thermal/thermal_zone" +
                            std::to_string(i) + "/type");
      std::string zone_type;
      max_zone = std::getline(type_in, zone_type) ? zone_type : "zone" + std::to_string(i);
    }
  }
  last_temp_zone_ = max_zone;
  return max_temp;
}

SystemSample SystemSampler::sample() {
  SystemSample s;
  s.cpu_pct = sample_cpu_pct();
  s.mem_pct = sample_mem_pct();
  s.temp_c = sample_temp_c();
  return s;
}

}  // namespace scheduler
}  // namespace jetedge
