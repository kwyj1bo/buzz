#include "buzz/validate.hpp"

#include <charconv>

namespace buzz {
namespace {

bool is_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '_' || c == '-';
}

bool valid_name(std::string_view name, size_t min, size_t max) {
  if (name.size() < min || name.size() > max) return false;
  for (char c : name) {
    if (!is_name_char(c)) return false;
  }
  return true;
}

template <typename T>
std::optional<T> parse_number(std::string_view text) {
  T value{};
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size() || text.empty()) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

bool valid_username(std::string_view name) { return valid_name(name, 3, 32); }

bool valid_password(std::string_view password) {
  return password.size() >= 8 && password.size() <= 128;
}

bool valid_room(std::string_view room) { return valid_name(room, 1, 32); }

bool valid_text(std::string_view text) {
  if (text.empty() || text.size() > kMaxTextSize) return false;
  for (char c : text) {
    auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f) return false;
  }
  return true;
}

std::optional<uint64_t> parse_u64(std::string_view text) { return parse_number<uint64_t>(text); }

std::optional<int64_t> parse_i64(std::string_view text) { return parse_number<int64_t>(text); }

}  // namespace buzz
