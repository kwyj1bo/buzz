#include "buzz/session_manager.hpp"

#include <stdexcept>

#include "buzz/crypto.hpp"

namespace buzz {
namespace {

constexpr size_t kTokenBytes = 32;

}  // namespace

SessionManager::SessionManager(std::chrono::seconds ttl) : ttl_(ttl) {}

Session& SessionManager::create(const std::string& user, uint64_t conn_id) {
  if (sessions_.count(user) != 0) throw std::logic_error("session already exists for " + user);
  Session session;
  session.token = crypto::to_hex(crypto::random_bytes(kTokenBytes));
  session.user = user;
  session.conn_id = conn_id;
  tokens_[session.token] = user;
  return sessions_.emplace(user, std::move(session)).first->second;
}

std::optional<Session> SessionManager::remove_user(const std::string& user) {
  auto it = sessions_.find(user);
  if (it == sessions_.end()) return std::nullopt;
  Session session = std::move(it->second);
  sessions_.erase(it);
  tokens_.erase(session.token);
  return session;
}

Session* SessionManager::find_user(const std::string& user) {
  auto it = sessions_.find(user);
  return it == sessions_.end() ? nullptr : &it->second;
}

Session* SessionManager::find_token(const std::string& token) {
  auto it = tokens_.find(token);
  return it == tokens_.end() ? nullptr : find_user(it->second);
}

void SessionManager::attach(const std::string& user, uint64_t conn_id) {
  if (Session* session = find_user(user)) session->conn_id = conn_id;
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

}  // namespace buzz
