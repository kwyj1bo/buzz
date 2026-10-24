#pragma once

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "buzz/crypto.hpp"

namespace buzz {

// Registered accounts with salted PBKDF2 password hashes.
// Thread-safe: the server calls it from its worker pool.
class UserStore {
 public:
  enum class RegisterResult { Ok, Exists, Invalid };

  explicit UserStore(uint32_t iterations = crypto::kDefaultPbkdf2Iterations);

  RegisterResult add_user(const std::string& name, const std::string& password);
  bool authenticate(const std::string& name, const std::string& password) const;
  size_t size() const;

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, crypto::PasswordHash> users_;
  uint32_t iterations_;
  // Hashed for unknown users, so a failed login takes the same time
  // whether or not the account exists.
  crypto::PasswordHash dummy_;
};

}  // namespace buzz
