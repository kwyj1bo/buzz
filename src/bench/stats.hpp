#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace buzz::bench {

using Clock = std::chrono::steady_clock;

inline int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

inline double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

// Latency samples in microseconds.
class Samples {
 public:
  void add(double micros) { values_.push_back(micros); }
  void merge(const Samples& other) {
    values_.insert(values_.end(), other.values_.begin(), other.values_.end());
  }
  size_t size() const { return values_.size(); }

  double percentile(double p) {
    if (values_.empty()) return 0;
    if (!sorted_) std::sort(values_.begin(), values_.end());
    sorted_ = true;
    size_t index = static_cast<size_t>(p / 100.0 * static_cast<double>(values_.size() - 1));
    return values_[index];
  }

 private:
  std::vector<double> values_;
  bool sorted_ = false;
};

// Prints a machine-readable result line that scripts/bench.sh collects.
inline void result(const std::string& key, double value, const std::string& unit) {
  std::printf("RESULT %s %.2f %s\n", key.c_str(), value, unit.c_str());
  std::fflush(stdout);
}

inline void report_latency(const std::string& key, Samples& samples) {
  result(key + "_p50", samples.percentile(50), "us");
  result(key + "_p95", samples.percentile(95), "us");
  result(key + "_p99", samples.percentile(99), "us");
  result(key + "_max", samples.percentile(100), "us");
}

// Resident memory of a process in KiB, from /proc. 0 when unknown.
inline long rss_kib(int pid) {
  if (pid <= 0) return 0;
  std::ifstream status("/proc/" + std::to_string(pid) + "/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmRSS:", 0) == 0) return std::stol(line.substr(6));
  }
  return 0;
}

// User plus system CPU time of a process in seconds. 0 when unknown.
inline double cpu_seconds(int pid) {
  if (pid <= 0) return 0;
  std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
  std::string content((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
  // Fields after the parenthesized command name; utime and stime are 14 and 15.
  auto close = content.rfind(')');
  if (close == std::string::npos) return 0;
  std::istringstream rest(content.substr(close + 2));
  std::string field;
  double ticks = 0;
  for (int i = 3; i <= 15 && rest >> field; ++i) {
    if (i >= 14) ticks += std::stod(field);
  }
  return ticks / static_cast<double>(sysconf(_SC_CLK_TCK));
}

}  // namespace buzz::bench
