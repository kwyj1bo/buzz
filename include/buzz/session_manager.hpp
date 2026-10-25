#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "buzz/net.hpp"

namespace buzz {

class Storage;

struct Session {
  std::string user;
  // SHA-256 of the token. The raw token lives only on the client, so a
  // copy of the database cannot be used to resume sessions.
  std::string token_hash;
  uint64_t conn_id = 0;  // 0 while detached.
  std::chrono::steady_clock::time_point detached_at{};
  std::optional<net::Endpoint> udp;  // Learned from a signed UDP hello.
  std::string udp_nonce;             // Fresh on every attach; sent in Welcome.
  uint64_t last_udp_counter = 0;     // Rejects replayed hellos.
};

// One session per user. A session outlives its TCP connection for `ttl`,
// so a client can reconnect with its token and resume. With a Storage,
// sessions also survive a server restart.
// Not thread-safe: owned by the server event loop.
class SessionManager {
 public:
  using Clock = std::chrono::steady_clock;

  explicit SessionManager(std::chrono::seconds ttl, Storage* storage = nullptr);

  // Creates a session and returns its raw token. The caller must remove any
  // earlier session of the user first.
  std::string create(const std::string& user, uint64_t conn_id);
  std::optional<Session> remove_user(const std::string& user);

  Session* find_user(const std::string& user);
  Session* find_token(const std::string& token);

  // Binds the session to a connection and starts a new UDP nonce.
  void attach(const std::string& user, uint64_t conn_id);
  void detach(const std::string& user, Clock::time_point now);
  // Removes sessions detached for longer than the ttl.
  std::vector<Session> expire(Clock::time_point now);

  size_t size() const { return sessions_.size(); }

 private:
  void start_udp_epoch(Session& session);
  void persist(const std::function<void()>& write);

  std::chrono::seconds ttl_;
  Storage* storage_;
  std::unordered_map<std::string, Session> sessions_;   // By user.
  std::unordered_map<std::string, std::string> tokens_;  // Token hash to user.
};

}  // namespace buzz
