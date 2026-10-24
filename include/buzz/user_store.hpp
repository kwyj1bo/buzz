#pragma once

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "buzz/crypto.hpp"
#include "buzz/storage.hpp"

namespace buzz {

// Registered accounts with salted PBKDF2 password hashes, cached in memory
// and written through to Storage when one is given.
// Thread-safe: the server calls it from its worker pool.
class UserStore {
 public:
  enum class RegisterResult { Ok, Exists, Invalid };

  explicit UserStore(uint32_t iterations = crypto::kDefaultPbkdf2Iterations,
                     Storage* storage = nullptr);

  RegisterResult add_user(const std::string& name, const std::string& password);
  bool authenticate(const std::string& name, const std::string& password) const;
  size_t size() const;

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, crypto::PasswordHash> users_;
  uint32_t iterations_;
  Storage* storage_;
  // Hashed for unknown users, so a failed login takes the same time
  // whether or not the account exists.
  crypto::PasswordHash dummy_;
};

}  // namespace buzz
