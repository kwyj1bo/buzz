#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace buzz::crypto {

constexpr size_t kKeySize = 32;
constexpr size_t kNonceSize = 12;
constexpr size_t kTagSize = 16;
constexpr size_t kSaltSize = 16;
constexpr size_t kHashSize = 32;
// OWASP 2023 guidance for PBKDF2-HMAC-SHA256.
constexpr uint32_t kDefaultPbkdf2Iterations = 600'000;

struct PasswordHash {
  std::string salt;
  std::string hash;
  uint32_t iterations = 0;
};

std::string random_bytes(size_t count);
std::string to_hex(std::string_view bytes);
std::optional<std::string> from_hex(std::string_view hex);
bool constant_time_equal(std::string_view a, std::string_view b);

PasswordHash hash_password(std::string_view password, uint32_t iterations);
bool verify_password(std::string_view password, const PasswordHash& stored);

std::string hmac_sha256(std::string_view key, std::string_view data);

// AES-256-GCM. Output layout: nonce | ciphertext | tag.
std::string seal(std::string_view key, std::string_view plaintext, std::string_view aad);
std::optional<std::string> open(std::string_view key, std::string_view sealed,
                                std::string_view aad);

// Overwrites the string contents before it is freed.
void wipe(std::string& secret);

}  // namespace buzz::crypto
