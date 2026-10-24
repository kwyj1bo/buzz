#include "buzz/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>

namespace buzz::log {
namespace {

std::atomic<Level> g_level{Level::Info};
std::mutex g_mutex;

const char* name(Level level) {
  switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
  }
  return "?";
}

std::string timestamp() {
  auto now = std::chrono::system_clock::now();
  std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char buffer[32];
  std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%S", &utc);
  char out[40];
  std::snprintf(out, sizeof out, "%s.%03dZ", buffer, static_cast<int>(millis));
  return out;
}

}  // namespace

void set_level(Level level) { g_level.store(level); }

Level level() { return g_level.load(); }

std::optional<Level> parse_level(std::string_view text) {
  if (text == "debug") return Level::Debug;
  if (text == "info") return Level::Info;
  if (text == "warn") return Level::Warn;
  if (text == "error") return Level::Error;
  return std::nullopt;
}

void write(Level level, const std::string& message) {
  std::lock_guard lock(g_mutex);
  std::cerr << timestamp() << ' ' << name(level) << ' ' << message << '\n';
}

}  // namespace buzz::log
