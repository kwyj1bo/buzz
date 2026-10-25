#include "buzz/auth_throttle.hpp"

#include <algorithm>
#include <iterator>

namespace buzz {
namespace {

constexpr auto kIdle = std::chrono::minutes(10);
constexpr auto kForgetFailures = std::chrono::hours(1);
constexpr auto kMaxLock = std::chrono::hours(1);

}  // namespace

bool AuthThrottle::allow(const std::string& ip, Clock::time_point now) {
  auto it = budgets_.find(ip);
  if (it == budgets_.end()) {
    it = budgets_.emplace(ip, Budget{TokenBucket(config_.per_minute / 60.0, config_.burst, now), now})
             .first;
  }
  it->second.last_used = now;
  return it->second.bucket.take(now);
}

AuthThrottle::Clock::duration AuthThrottle::locked_for(const std::string& user,
                                                       Clock::time_point now) const {
  auto it = failures_.find(user);
  if (it == failures_.end() || it->second.locked_until <= now) return Clock::duration::zero();
  return it->second.locked_until - now;
}

void AuthThrottle::record_failure(const std::string& user, Clock::time_point now) {
  Failures& f = failures_[user];
  f.count++;
  f.last = now;
  int over = f.count - config_.lockout_failures;
  if (over < 0) return;
  auto lock = std::chrono::duration_cast<Clock::duration>(config_.lockout) * (1LL << std::min(over, 10));
  f.locked_until = now + std::min<Clock::duration>(lock, kMaxLock);
}

void AuthThrottle::prune(Clock::time_point now) {
  for (auto it = budgets_.begin(); it != budgets_.end();) {
    it = now - it->second.last_used > kIdle ? budgets_.erase(it) : std::next(it);
  }
  for (auto it = failures_.begin(); it != failures_.end();) {
    bool stale = now - it->second.last > kForgetFailures && it->second.locked_until <= now;
    it = stale ? failures_.erase(it) : std::next(it);
  }
}

}  // namespace buzz
