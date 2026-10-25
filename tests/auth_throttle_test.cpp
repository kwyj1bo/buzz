// Bug 3: limits on password work per IP address and per username.
#include <gtest/gtest.h>

#include "buzz/auth_throttle.hpp"

namespace buzz {
namespace {

using namespace std::chrono_literals;
using Clock = AuthThrottle::Clock;

AuthThrottle::Config config() {
  AuthThrottle::Config c;
  c.per_minute = 60;  // One per second.
  c.burst = 2;
  c.lockout_failures = 3;
  c.lockout = 60s;
  return c;
}

TEST(AuthThrottle, BudgetIsPerAddress) {
  AuthThrottle throttle(config());
  auto t0 = Clock::now();
  EXPECT_TRUE(throttle.allow("10.0.0.1", t0));
  EXPECT_TRUE(throttle.allow("10.0.0.1", t0));
  EXPECT_FALSE(throttle.allow("10.0.0.1", t0));
  EXPECT_TRUE(throttle.allow("10.0.0.2", t0));
  EXPECT_TRUE(throttle.allow("10.0.0.1", t0 + 1s));
}

TEST(AuthThrottle, LocksUserAfterRepeatedFailures) {
  AuthThrottle throttle(config());
  auto t0 = Clock::now();
  throttle.record_failure("alice", t0);
  throttle.record_failure("alice", t0);
  EXPECT_EQ(throttle.locked_for("alice", t0), Clock::duration::zero());
  throttle.record_failure("alice", t0);
  EXPECT_GT(throttle.locked_for("alice", t0), 59s);
  EXPECT_EQ(throttle.locked_for("alice", t0 + 61s), Clock::duration::zero());
  EXPECT_EQ(throttle.locked_for("bob", t0), Clock::duration::zero());
}

TEST(AuthThrottle, LockDoublesAndIsCapped) {
  AuthThrottle throttle(config());
  auto t0 = Clock::now();
  for (int i = 0; i < 4; ++i) throttle.record_failure("alice", t0);
  EXPECT_GT(throttle.locked_for("alice", t0), 119s);
  for (int i = 0; i < 20; ++i) throttle.record_failure("alice", t0);
  EXPECT_LE(throttle.locked_for("alice", t0), std::chrono::hours(1));
}

TEST(AuthThrottle, SuccessClearsFailures) {
  AuthThrottle throttle(config());
  auto t0 = Clock::now();
  throttle.record_failure("alice", t0);
  throttle.record_failure("alice", t0);
  throttle.record_success("alice");
  throttle.record_failure("alice", t0);
  EXPECT_EQ(throttle.locked_for("alice", t0), Clock::duration::zero());
}

TEST(AuthThrottle, PruneForgetsIdleState) {
  AuthThrottle throttle(config());
  auto t0 = Clock::now();
  throttle.allow("10.0.0.1", t0);
  throttle.allow("10.0.0.1", t0);
  throttle.prune(t0 + std::chrono::minutes(11));
  EXPECT_TRUE(throttle.allow("10.0.0.1", t0 + std::chrono::minutes(11)));
}

}  // namespace
}  // namespace buzz
