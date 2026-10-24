#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace buzz {

constexpr size_t kMaxTextSize = 4096;

// 3 to 32 characters: letters, digits, '_' or '-'.
bool valid_username(std::string_view name);
// 8 to 128 bytes.
bool valid_password(std::string_view password);
// 1 to 32 characters: letters, digits, '_' or '-'.
bool valid_room(std::string_view room);
// 1 to kMaxTextSize bytes with no control characters, so text cannot inject
// terminal escape sequences into other clients.
bool valid_text(std::string_view text);

std::optional<uint64_t> parse_u64(std::string_view text);
std::optional<int64_t> parse_i64(std::string_view text);

}  // namespace buzz
