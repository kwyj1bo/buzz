#include "buzz/session_manager.hpp"

#include <stdexcept>

#include "buzz/crypto.hpp"
#include "buzz/log.hpp"
#include "buzz/storage.hpp"

namespace buzz {
namespace {

constexpr size_t kTokenBytes = 32;
constexpr size_t kNonceBytes = 16;

}  // namespace

SessionManager::SessionManager(std::chrono::seconds ttl, Storage* storage)
    : ttl_(ttl), storage_(storage) {
  if (!storage_) return;
  // Restored sessions start detached; their owners have ttl to resume.
  auto now = Clock::now();
  for (auto& [user, token_hash] : storage_->load_sessions()) {
    Session session;
    session.user = user;
    session.token_hash = token_hash;
    session.detached_at = now;
    tokens_[token_hash] = user;
    sessions_.emplace(user, std::move(session));
  }
}

std::string SessionManager::create(const std::string& user, uint64_t conn_id) {
  if (sessions_.count(user) != 0) throw std::logic_error("session already exists for " + user);
  std::string token = crypto::to_hex(crypto::random_bytes(kTokenBytes));
  Session session;
  session.user = user;
  session.token_hash = crypto::sha256(token);
  session.conn_id = conn_id;
  start_udp_epoch(session);
  tokens_[session.token_hash] = user;
  persist([&] { storage_->save_session(user, session.token_hash); });
  sessions_.emplace(user, std::move(session));
  return token;
}

std::optional<Session> SessionManager::remove_user(const std::string& user) {
  auto it = sessions_.find(user);
  if (it == sessions_.end()) return std::nullopt;
  Session session = std::move(it->second);
  sessions_.erase(it);
  tokens_.erase(session.token_hash);
  persist([&] { storage_->delete_session(user); });
  return session;
}

Session* SessionManager::find_user(const std::string& user) {
  auto it = sessions_.find(user);
  return it == sessions_.end() ? nullptr : &it->second;
}

Session* SessionManager::find_token(const std::string& token) {
  auto it = tokens_.find(crypto::sha256(token));
  return it == tokens_.end() ? nullptr : find_user(it->second);
}

void SessionManager::attach(const std::string& user, uint64_t conn_id) {
  Session* session = find_user(user);
  if (!session) return;
  session->conn_id = conn_id;
  start_udp_epoch(*session);
}

void SessionManager::detach(const std::string& user, Clock::time_point now) {
  Session* session = find_user(user);
  if (!session) return;
  session->conn_id = 0;
  session->detached_at = now;
  // The client NAT mapping may change while away; it re-registers on resume.
  session->udp.reset();
}

std::vector<Session> SessionManager::expire(Clock::time_point now) {
  std::vector<std::string> stale;
  for (const auto& [user, session] : sessions_) {
    if (session.conn_id == 0 && now - session.detached_at >= ttl_) stale.push_back(user);
  }
  std::vector<Session> removed;
  removed.reserve(stale.size());
  for (const auto& user : stale) removed.push_back(*remove_user(user));
  return removed;
}

// Sessions still work in memory when the database cannot be written.
void SessionManager::persist(const std::function<void()>& write) {
  if (!storage_) return;
  try {
    write();
  } catch (const StorageError& e) {
    BUZZ_LOG(Warn, "could not persist session: " << e.what());
  }
}

// A new nonce invalidates every hello signed for an earlier connection, so
// the counter can restart at zero without allowing replays.
void SessionManager::start_udp_epoch(Session& session) {
  session.udp.reset();
  session.udp_nonce = crypto::to_hex(crypto::random_bytes(kNonceBytes));
  session.last_udp_counter = 0;
}

}  // namespace buzz
