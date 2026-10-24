#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "buzz/net.hpp"

namespace buzz {

struct Session {
  std::string token;
  std::string user;
  uint64_t conn_id = 0;  // 0 while detached.
  std::chrono::steady_clock::time_point detached_at{};
  std::optional<net::Endpoint> udp;  // Learned from a signed UDP hello.
  int64_t last_udp_timestamp = 0;    // Rejects replayed hellos.
};

// One session per user. A session outlives its TCP connection for `ttl`,
// so a client can reconnect with its token and resume.
// Not thread-safe: owned by the server event loop.
class SessionManager {
 public:
  using Clock = std::chrono::steady_clock;

  explicit SessionManager(std::chrono::seconds ttl);

  // The caller must remove any earlier session of the user first.
  Session& create(const std::string& user, uint64_t conn_id);
  std::optional<Session> remove_user(const std::string& user);

  Session* find_user(const std::string& user);
  Session* find_token(const std::string& token);

  void attach(const std::string& user, uint64_t conn_id);
  void detach(const std::string& user, Clock::time_point now);
  // Removes sessions detached for longer than the ttl.
  std::vector<Session> expire(Clock::time_point now);

  size_t size() const { return sessions_.size(); }

 private:
  std::chrono::seconds ttl_;
  std::unordered_map<std::string, Session> sessions_;   // By user.
  std::unordered_map<std::string, std::string> tokens_;  // Token to user.
};

}  // namespace buzz
