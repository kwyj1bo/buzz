#pragma once

#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace buzz::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_level(Level level);
Level level();
std::optional<Level> parse_level(std::string_view name);
void write(Level level, const std::string& message);

}  // namespace buzz::log

#define BUZZ_LOG(LEVEL, EXPR)                                                \
  do {                                                                       \
    if (::buzz::log::Level::LEVEL >= ::buzz::log::level()) {                 \
      std::ostringstream buzz_log_stream_;                                   \
      buzz_log_stream_ << EXPR;                                              \
      ::buzz::log::write(::buzz::log::Level::LEVEL, buzz_log_stream_.str()); \
    }                                                                        \
  } while (0)
