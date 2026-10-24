#include <gtest/gtest.h>

#include "buzz/session_manager.hpp"

namespace buzz {
namespace {

using namespace std::chrono_literals;

TEST(SessionManager, CreatesAndFinds) {
  SessionManager sessions(60s);
  Session& s = sessions.create("alice", 7);
  EXPECT_EQ(s.token.size(), 64u);
  EXPECT_EQ(sessions.find_user("alice"), &s);
  EXPECT_EQ(sessions.find_token(s.token), &s);
  EXPECT_EQ(sessions.find_token("nope"), nullptr);
}

TEST(SessionManager, TokensAreUnique) {
  SessionManager sessions(60s);
  std::string a = sessions.create("alice", 1).token;
  std::string b = sessions.create("bob", 2).token;
  EXPECT_NE(a, b);
}

TEST(SessionManager, CreateTwiceIsALogicError) {
  SessionManager sessions(60s);
  sessions.create("alice", 1);
  EXPECT_THROW(sessions.create("alice", 2), std::logic_error);
}

TEST(SessionManager, RemoveDropsToken) {
  SessionManager sessions(60s);
  std::string token = sessions.create("alice", 1).token;
  auto removed = sessions.remove_user("alice");
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(removed->token, token);
  EXPECT_EQ(sessions.find_token(token), nullptr);
}

TEST(SessionManager, DetachedSessionExpiresAfterTtl) {
  SessionManager sessions(10s);
  sessions.create("alice", 1);
  sessions.create("bob", 2);
  auto t0 = SessionManager::Clock::now();
  sessions.detach("alice", t0);

  EXPECT_TRUE(sessions.expire(t0 + 9s).empty());
  auto expired = sessions.expire(t0 + 10s);
  ASSERT_EQ(expired.size(), 1u);
  EXPECT_EQ(expired[0].user, "alice");
  EXPECT_NE(sessions.find_user("bob"), nullptr);
}

TEST(SessionManager, AttachStopsExpiry) {
  SessionManager sessions(10s);
  sessions.create("alice", 1);
  auto t0 = SessionManager::Clock::now();
  sessions.detach("alice", t0);
  sessions.attach("alice", 5);
  EXPECT_TRUE(sessions.expire(t0 + 60s).empty());
  EXPECT_EQ(sessions.find_user("alice")->conn_id, 5u);
}

TEST(SessionManager, DetachForgetsUdpEndpoint) {
  SessionManager sessions(10s);
  sessions.create("alice", 1).udp = net::Endpoint{"10.0.0.1", 4000};
  sessions.detach("alice", SessionManager::Clock::now());
  EXPECT_FALSE(sessions.find_user("alice")->udp.has_value());
}

}  // namespace
}  // namespace buzz
