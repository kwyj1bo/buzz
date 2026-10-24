#include "buzz/user_store.hpp"

#include <mutex>
#include <optional>

#include "buzz/validate.hpp"

namespace buzz {

UserStore::UserStore(uint32_t iterations, Storage* storage)
    : iterations_(iterations),
      storage_(storage),
      dummy_(crypto::hash_password("buzz-dummy-password", iterations)) {
  if (!storage_) return;
  for (auto& [name, hash] : storage_->load_users()) users_.emplace(name, std::move(hash));
}

UserStore::RegisterResult UserStore::add_user(const std::string& name,
                                              const std::string& password) {
  if (!valid_username(name) || !valid_password(password)) return RegisterResult::Invalid;
  {
    std::shared_lock lock(mutex_);
    if (users_.count(name) != 0) return RegisterResult::Exists;
  }
  // Hash outside the lock: it is the slow part.
  crypto::PasswordHash hash = crypto::hash_password(password, iterations_);
  std::unique_lock lock(mutex_);
  if (users_.count(name) != 0) return RegisterResult::Exists;
  if (storage_ && !storage_->insert_user(name, hash)) return RegisterResult::Exists;
  users_.emplace(name, std::move(hash));
  return RegisterResult::Ok;
}

bool UserStore::authenticate(const std::string& name, const std::string& password) const {
  std::optional<crypto::PasswordHash> stored;
  {
    std::shared_lock lock(mutex_);
    auto it = users_.find(name);
    if (it != users_.end()) stored = it->second;
  }
  if (!stored) {
    crypto::verify_password(password, dummy_);
    return false;
  }
  return crypto::verify_password(password, *stored);
}

size_t UserStore::size() const {
  std::shared_lock lock(mutex_);
  return users_.size();
}

}  // namespace buzz
