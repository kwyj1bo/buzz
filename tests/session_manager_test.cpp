#include <gtest/gtest.h>
#include <stdlib.h>

#include <filesystem>

#include "buzz/crypto.hpp"
#include "buzz/session_manager.hpp"
#include "buzz/storage.hpp"

namespace buzz {
namespace {

using namespace std::chrono_literals;

TEST(SessionManager, CreatesAndFinds) {
  SessionManager sessions(60s);
  std::string token = sessions.create("alice", 7);
  EXPECT_EQ(token.size(), 64u);
  Session* s = sessions.find_user("alice");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(sessions.find_token(token), s);
  EXPECT_EQ(sessions.find_token("nope"), nullptr);
}

TEST(SessionManager, StoresOnlyTheTokenHash) {
  SessionManager sessions(60s);
  std::string token = sessions.create("alice", 1);
  EXPECT_EQ(sessions.find_user("alice")->token_hash, crypto::sha256(token));
}

TEST(SessionManager, TokensAreUnique) {
  SessionManager sessions(60s);
  EXPECT_NE(sessions.create("alice", 1), sessions.create("bob", 2));
}

TEST(SessionManager, CreateTwiceIsALogicError) {
  SessionManager sessions(60s);
  sessions.create("alice", 1);
  EXPECT_THROW(sessions.create("alice", 2), std::logic_error);
}

TEST(SessionManager, RemoveDropsToken) {
  SessionManager sessions(60s);
  std::string token = sessions.create("alice", 1);
  ASSERT_TRUE(sessions.remove_user("alice").has_value());
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

// Bug 9: UDP hellos are bound to a nonce that changes on every attach, not
// to the client's clock.
TEST(SessionManager, AttachStartsNewUdpEpoch) {
  SessionManager sessions(10s);
  sessions.create("alice", 1);
  Session* s = sessions.find_user("alice");
  s->udp = net::Endpoint{"10.0.0.1", 4000};
  s->last_udp_counter = 42;
  std::string nonce = s->udp_nonce;
  sessions.attach("alice", 2);
  EXPECT_NE(s->udp_nonce, nonce);
  EXPECT_EQ(s->last_udp_counter, 0u);
  EXPECT_FALSE(s->udp.has_value());
}

TEST(SessionManager, DetachForgetsUdpEndpoint) {
  SessionManager sessions(10s);
  sessions.create("alice", 1);
  sessions.find_user("alice")->udp = net::Endpoint{"10.0.0.1", 4000};
  sessions.detach("alice", SessionManager::Clock::now());
  EXPECT_FALSE(sessions.find_user("alice")->udp.has_value());
}

// Bug 9: sessions survive a restart when there is a database.
TEST(SessionManager, SessionsPersistAcrossRestart) {
  std::string pattern = ::testing::TempDir() + "buzz-sessions-XXXXXX";
  std::string dir = ::mkdtemp(pattern.data());
  std::string token;
  {
    Storage storage(dir + "/buzz.db");
    SessionManager sessions(60s, &storage);
    token = sessions.create("alice", 1);
    sessions.create("bob", 2);
    sessions.remove_user("bob");
  }
  Storage storage(dir + "/buzz.db");
  SessionManager sessions(60s, &storage);
  Session* s = sessions.find_token(token);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->user, "alice");
  EXPECT_EQ(s->conn_id, 0u);  // Restored detached.
  EXPECT_EQ(sessions.find_user("bob"), nullptr);
  std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace buzz
