#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

#include "buzz/token_bucket.hpp"

namespace buzz {

// Limits password work that a client can trigger. Each IP address gets a
// token bucket for Register and Login, since each one costs a PBKDF2 hash.
// Each username is locked after repeated failed logins, with the lock
// doubling up to an hour, so reconnecting does not reset the count.
// Not thread-safe: owned by the server event loop.
class AuthThrottle {
 public:
  using Clock = std::chrono::steady_clock;

  struct Config {
    double per_minute = 20;
    double burst = 10;
    int lockout_failures = 5;
    std::chrono::seconds lockout{60};
  };

  explicit AuthThrottle(Config config) : config_(config) {}

  // Takes one attempt from the address's budget.
  bool allow(const std::string& ip, Clock::time_point now);
  // Time left on the user's lock; zero when not locked.
  Clock::duration locked_for(const std::string& user, Clock::time_point now) const;
  void record_failure(const std::string& user, Clock::time_point now);
  void record_success(const std::string& user) { failures_.erase(user); }
  // Forgets idle addresses and old failures.
  void prune(Clock::time_point now);

 private:
  struct Budget {
    TokenBucket bucket;
    Clock::time_point last_used;
  };
  struct Failures {
    int count = 0;
    Clock::time_point last{};
    Clock::time_point locked_until{};
  };

  Config config_;
  std::unordered_map<std::string, Budget> budgets_;
  std::unordered_map<std::string, Failures> failures_;
};

}  // namespace buzz
