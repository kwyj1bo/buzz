#pragma once

#include <openssl/types.h>

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

std::string sha256(std::string_view data);
std::string hmac_sha256(std::string_view key, std::string_view data);
std::string hkdf_sha256(std::string_view secret, std::string_view salt, std::string_view info,
                        size_t length);

// AES-256-GCM. Output layout: nonce | ciphertext | tag.
std::string seal(std::string_view key, std::string_view plaintext, std::string_view aad);
std::optional<std::string> open(std::string_view key, std::string_view sealed,
                                std::string_view aad);

// Overwrites the string contents before it is freed.
void wipe(std::string& secret);

// Ephemeral X25519 key pair for Diffie-Hellman between peers.
class X25519KeyPair {
 public:
  static constexpr size_t kPublicSize = 32;

  X25519KeyPair();
  ~X25519KeyPair();
  X25519KeyPair(const X25519KeyPair&) = delete;
  X25519KeyPair& operator=(const X25519KeyPair&) = delete;

  // Raw 32-byte public key.
  const std::string& public_key() const { return public_; }
  // Shared secret with a peer's raw public key. Nullopt for an invalid key.
  std::optional<std::string> agree(std::string_view peer_public) const;

 private:
  EVP_PKEY* key_;
  std::string public_;
};

}  // namespace buzz::crypto
