// Relay fallback, rate limiting and connection limits on a real server.
#include <gtest/gtest.h>
#include <poll.h>
#include <unistd.h>

#include <thread>

#include "buzz/token_bucket.hpp"
#include "support/test_client.hpp"
#include "support/test_server.hpp"

namespace buzz::test {
namespace {

TEST(TokenBucket, AllowsBurstThenRefills) {
  auto t0 = TokenBucket::Clock::now();
  TokenBucket bucket(10, 3, t0);
  EXPECT_TRUE(bucket.take(t0));
  EXPECT_TRUE(bucket.take(t0));
  EXPECT_TRUE(bucket.take(t0));
  EXPECT_FALSE(bucket.take(t0));
  EXPECT_TRUE(bucket.take(t0 + 100ms));  // One token per 100 ms.
  EXPECT_FALSE(bucket.take(t0 + 100ms));
  EXPECT_TRUE(bucket.take(t0 + 10s));    // Refill caps at the burst size.
  EXPECT_TRUE(bucket.take(t0 + 10s));
  EXPECT_TRUE(bucket.take(t0 + 10s));
  EXPECT_FALSE(bucket.take(t0 + 10s));
}

TEST(Relay, DeliversThroughServer) {
  TestServer server;
  TestClient alice(server.cert_file(), server.port());
  TestClient bob(server.cert_file(), server.port());
  alice.sign_up("alice");
  bob.sign_up("bob");
  alice.send({MsgType::Relay, {"bob", "via the server"}});
  auto ok = alice.expect(MsgType::Ok);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok->field(0), "relay");
  auto got = bob.expect(MsgType::Relayed);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->field(0), "alice");
  EXPECT_EQ(got->field(1), "via the server");
}

TEST(Relay, OfflineTargetFails) {
  TestServer server;
  TestClient alice(server.cert_file(), server.port());
  alice.sign_up("alice");
  alice.send({MsgType::Relay, {"ghost", "hello?"}});
  auto reply = alice.expect(MsgType::Error);
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->field(1), "ghost is not online");
  EXPECT_EQ(reply->field(2), "ghost");
}

TEST(RateLimit, RejectsFloodButKeepsConnection) {
  TestServer server("", [](ServerConfig& c) {
    c.message_rate = 0.001;
    c.message_burst = 4;  // One Join plus three messages.
  });
  TestClient alice(server.cert_file(), server.port());
  alice.sign_up("alice");
  alice.send({MsgType::Join, {"general"}});
  ASSERT_TRUE(alice.expect(MsgType::SyncDone).has_value());
  for (int i = 0; i < 5; ++i) alice.send({MsgType::Say, {"general", "spam"}});
  int delivered = 0;
  int limited = 0;
  while (delivered + limited < 5) {
    auto m = alice.receive();
    ASSERT_TRUE(m.has_value());
    if (m->type == MsgType::RoomMsg) ++delivered;
    if (m->type == MsgType::Error && m->field(1).find("rate limit") != std::string::npos) ++limited;
  }
  EXPECT_EQ(delivered, 3);
  EXPECT_EQ(limited, 2);
  alice.send({MsgType::Ping, {"still here"}});
  EXPECT_TRUE(alice.expect(MsgType::Pong).has_value());
}

TEST(BruteForce, FailedLoginsCloseConnection) {
  TestServer server("", [](ServerConfig& c) { c.max_failed_logins = 3; });
  TestClient setup(server.cert_file(), server.port());
  setup.sign_up("alice");
  TestClient attacker(server.cert_file(), server.port());
  for (int i = 0; i < 3; ++i) {
    attacker.send({MsgType::Login, {"alice", "guess" + std::to_string(i) + "xx"}});
    ASSERT_TRUE(attacker.expect(MsgType::Error).has_value());
  }
  EXPECT_TRUE(attacker.closed_by_server());
}

TEST(Timeouts, IdleSocketWithoutHandshakeIsClosed) {
  TestServer server("", [](ServerConfig& c) { c.handshake_timeout = std::chrono::seconds(1); });
  auto fd = net::tcp_connect("127.0.0.1", server.port(), 5s);
  pollfd pfd{fd.get(), POLLIN, 0};
  // The sweep runs once a second, so allow up to about three seconds.
  ASSERT_EQ(::poll(&pfd, 1, 5000), 1);
  char byte;
  EXPECT_EQ(::read(fd.get(), &byte, 1), 0);
}

TEST(Timeouts, LoggedInClientStays) {
  TestServer server("", [](ServerConfig& c) {
    c.handshake_timeout = std::chrono::seconds(1);
    c.login_timeout = std::chrono::seconds(1);
  });
  TestClient alice(server.cert_file(), server.port());
  alice.sign_up("alice");
  std::this_thread::sleep_for(2500ms);
  alice.send({MsgType::Ping, {"1"}});
  EXPECT_TRUE(alice.expect(MsgType::Pong).has_value());
}

// Bug 3: failed logins count per username, so reconnecting does not reset
// them, and the lock holds even for the right password.
TEST(BruteForce, UsernameLockSurvivesReconnects) {
  TestServer server("", [](ServerConfig& c) {
    c.auth.lockout_failures = 2;
    c.max_failed_logins = 100;
  });
  TestClient setup(server.cert_file(), server.port());
  setup.sign_up("alice");
  for (int i = 0; i < 2; ++i) {
    TestClient attacker(server.cert_file(), server.port());
    attacker.send({MsgType::Login, {"alice", "wrong-password"}});
    ASSERT_TRUE(attacker.expect(MsgType::Error).has_value());
  }
  TestClient again(server.cert_file(), server.port());
  again.send({MsgType::Login, {"alice", "password123"}});
  auto reply = again.expect(MsgType::Error);
  ASSERT_TRUE(reply.has_value());
  EXPECT_NE(reply->field(1).find("too many failed logins"), std::string::npos);
}

// Bug 3: registration draws from a per-IP budget, across connections.
TEST(BruteForce, RegistrationsAreLimitedPerAddress) {
  TestServer server("", [](ServerConfig& c) {
    c.auth.per_minute = 0.001;
    c.auth.burst = 2;
  });
  int accepted = 0;
  for (int i = 0; i < 4; ++i) {
    TestClient client(server.cert_file(), server.port());
    client.send({MsgType::Register, {"user" + std::to_string(i), "password123"}});
    auto reply = client.receive();
    ASSERT_TRUE(reply.has_value());
    if (reply->type == MsgType::Ok) ++accepted;
    else EXPECT_NE(reply->field(1).find("too many attempts"), std::string::npos);
  }
  EXPECT_EQ(accepted, 2);
}

// Bug 2: one user cannot hold every room slot.
TEST(Rooms, PerUserRoomLimitProtectsRoomCreation) {
  TestServer server("", [](ServerConfig& c) {
    c.max_rooms = 3;
    c.max_rooms_per_user = 2;
  });
  TestClient mallory(server.cert_file(), server.port());
  mallory.sign_up("mallory");
  for (const char* room : {"r1", "r2", "r3"}) mallory.send({MsgType::Join, {room}});
  ASSERT_TRUE(mallory.expect(MsgType::SyncDone).has_value());
  ASSERT_TRUE(mallory.expect(MsgType::SyncDone).has_value());
  auto refused = mallory.expect(MsgType::Error);
  ASSERT_TRUE(refused.has_value());
  EXPECT_NE(refused->field(1).find("too many rooms"), std::string::npos);

  TestClient alice(server.cert_file(), server.port());
  alice.sign_up("alice");
  alice.send({MsgType::Join, {"general"}});
  EXPECT_TRUE(alice.expect(MsgType::SyncDone).has_value());
}

}  // namespace
}  // namespace buzz::test
