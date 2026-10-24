#include "buzz/crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <stdexcept>

namespace buzz::crypto {
namespace {

const unsigned char* bytes(std::string_view s) {
  return reinterpret_cast<const unsigned char*>(s.data());
}

unsigned char* bytes(std::string& s) { return reinterpret_cast<unsigned char*>(s.data()); }

int as_int(size_t size) { return static_cast<int>(size); }

class CipherContext {
 public:
  CipherContext() : ctx_(EVP_CIPHER_CTX_new()) {
    if (!ctx_) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
  }
  ~CipherContext() { EVP_CIPHER_CTX_free(ctx_); }
  CipherContext(const CipherContext&) = delete;
  CipherContext& operator=(const CipherContext&) = delete;
  EVP_CIPHER_CTX* get() const { return ctx_; }

 private:
  EVP_CIPHER_CTX* ctx_;
};

void check(int ok, const char* what) {
  if (ok != 1) throw std::runtime_error(what);
}

std::string pbkdf2(std::string_view password, std::string_view salt, uint32_t iterations) {
  std::string out(kHashSize, '\0');
  check(PKCS5_PBKDF2_HMAC(password.data(), as_int(password.size()), bytes(salt),
                          as_int(salt.size()), static_cast<int>(iterations), EVP_sha256(),
                          as_int(out.size()), bytes(out)),
        "PBKDF2 failed");
  return out;
}

int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

PasswordHash hash_password(std::string_view password, uint32_t iterations) {
  std::string salt = random_bytes(kSaltSize);
  std::string hash = pbkdf2(password, salt, iterations);
  return {std::move(salt), std::move(hash), iterations};
}

bool verify_password(std::string_view password, const PasswordHash& stored) {
  return constant_time_equal(pbkdf2(password, stored.salt, stored.iterations), stored.hash);
}

std::string hmac_sha256(std::string_view key, std::string_view data) {
  unsigned char out[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (!HMAC(EVP_sha256(), key.data(), as_int(key.size()), bytes(data), data.size(), out,
            &length)) {
    throw std::runtime_error("HMAC failed");
  }
  return std::string(reinterpret_cast<char*>(out), length);
}

std::string seal(std::string_view key, std::string_view plaintext, std::string_view aad) {
  if (key.size() != kKeySize) throw std::invalid_argument("AES-256-GCM needs a 32 byte key");
  std::string out = random_bytes(kNonceSize);
  out.resize(kNonceSize + plaintext.size() + kTagSize);
  unsigned char* ciphertext = bytes(out) + kNonceSize;

  CipherContext ctx;
  int length = 0;
  check(EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr), "init");
  check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr), "ivlen");
  check(EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, bytes(key), bytes(out)), "key");
  if (!aad.empty()) {
    check(EVP_EncryptUpdate(ctx.get(), nullptr, &length, bytes(aad), as_int(aad.size())), "aad");
  }
  int written = 0;
  if (!plaintext.empty()) {
    check(EVP_EncryptUpdate(ctx.get(), ciphertext, &written, bytes(plaintext),
                            as_int(plaintext.size())),
          "encrypt");
  }
  check(EVP_EncryptFinal_ex(ctx.get(), ciphertext + written, &length), "final");
  check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, kTagSize,
                            ciphertext + plaintext.size()),
        "tag");
  return out;
}

std::optional<std::string> open(std::string_view key, std::string_view sealed,
                                std::string_view aad) {
  if (key.size() != kKeySize || sealed.size() < kNonceSize + kTagSize) return std::nullopt;
  std::string_view nonce = sealed.substr(0, kNonceSize);
  std::string_view ciphertext = sealed.substr(kNonceSize, sealed.size() - kNonceSize - kTagSize);
  std::string tag(sealed.substr(sealed.size() - kTagSize));
  std::string plaintext(ciphertext.size(), '\0');

  CipherContext ctx;
  int length = 0;
  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) != 1 ||
      EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, bytes(key), bytes(nonce)) != 1) {
    return std::nullopt;
  }
  if (!aad.empty() &&
      EVP_DecryptUpdate(ctx.get(), nullptr, &length, bytes(aad), as_int(aad.size())) != 1) {
    return std::nullopt;
  }
  int written = 0;
  if (!ciphertext.empty() &&
      EVP_DecryptUpdate(ctx.get(), bytes(plaintext), &written, bytes(ciphertext),
                        as_int(ciphertext.size())) != 1) {
    return std::nullopt;
  }
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, kTagSize, tag.data()) != 1) {
    return std::nullopt;
  }
  if (EVP_DecryptFinal_ex(ctx.get(), bytes(plaintext) + written, &length) <= 0) {
    return std::nullopt;
  }
  return plaintext;
}

std::string random_bytes(size_t count) {
  std::string out(count, '\0');
  if (count > 0 && RAND_bytes(bytes(out), as_int(count)) != 1) {
    throw std::runtime_error("RAND_bytes failed");
  }
  return out;
}

std::string to_hex(std::string_view in) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(in.size() * 2);
  for (unsigned char c : in) {
    out.push_back(kDigits[c >> 4]);
    out.push_back(kDigits[c & 0x0f]);
  }
  return out;
}

std::optional<std::string> from_hex(std::string_view in) {
  if (in.size() % 2 != 0) return std::nullopt;
  std::string out;
  out.reserve(in.size() / 2);
  for (size_t i = 0; i < in.size(); i += 2) {
    int hi = nibble(in[i]);
    int lo = nibble(in[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out.push_back(static_cast<char>(hi << 4 | lo));
  }
  return out;
}

bool constant_time_equal(std::string_view a, std::string_view b) {
  return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

void wipe(std::string& secret) {
  if (!secret.empty()) OPENSSL_cleanse(secret.data(), secret.size());
  secret.clear();
}

}  // namespace buzz::crypto
