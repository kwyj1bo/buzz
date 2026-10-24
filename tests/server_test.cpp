// End-to-end tests: a real server on loopback, driven over TLS and UDP.
#include <gtest/gtest.h>
#include <poll.h>

#include <memory>
#include <vector>

#include "buzz/crypto.hpp"
#include "buzz/p2p.hpp"
#include "support/test_client.hpp"
#include "support/test_server.hpp"

namespace buzz::test {
namespace {

class ServerTest : public ::testing::Test {
 protected:
  std::unique_ptr<TestClient> connect() {
    return std::make_unique<TestClient>(server_.cert_file(), server_.port());
  }

  // Logs in a fresh user and joins `room`.
  std::unique_ptr<TestClient> member(const std::string& user, const std::string& room) {
    auto client = connect();
    client->sign_up(user);
    client->send({MsgType::Join, {room}});
    EXPECT_TRUE(client->expect(MsgType::SyncDone).has_value());
    return client;
  }

  TestServer server_;
};

TEST_F(ServerTest, RegisterLoginAndChat) {
  auto alice = member("alice", "general");
  auto bob = member("bob", "general");
  auto joined = alice->expect(MsgType::RoomEvent);
  ASSERT_TRUE(joined.has_value());
  EXPECT_EQ(joined->fields, (std::vector<std::string>{"general", "join", "bob"}));

  alice->send({MsgType::Say, {"general", "hi bob"}});
  auto got = bob->expect(MsgType::RoomMsg);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->field(0), "general");
  EXPECT_EQ(got->field(1), "1");
  EXPECT_EQ(got->field(3), "alice");
  EXPECT_EQ(got->field(4), "hi bob");
  auto echo = alice->expect(MsgType::RoomMsg);
  ASSERT_TRUE(echo.has_value());
  EXPECT_EQ(echo->field(4), "hi bob");
}

TEST_F(ServerTest, WrongPasswordIsRejected) {
  auto client = connect();
  client->sign_up("alice");
  auto other = connect();
  other->send({MsgType::Login, {"alice", "not-the-password"}});
  auto reply = other->receive();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->type, MsgType::Error);
  EXPECT_EQ(reply->field(0), "login");
}

TEST_F(ServerTest, DuplicateRegistrationFails) {
  auto client = connect();
  client->sign_up("alice");
  client->send({MsgType::Register, {"alice", "password123"}});
  auto reply = client->receive();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->type, MsgType::Error);
  EXPECT_EQ(reply->field(1), "username is taken");
}

TEST_F(ServerTest, CommandsRequireLogin) {
  auto client = connect();
  client->send({MsgType::Join, {"general"}});
  auto reply = client->receive();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->type, MsgType::Error);
  EXPECT_EQ(reply->field(1), "not logged in");
}

TEST_F(ServerTest, SayRejectsControlCharacters) {
  auto alice = member("alice", "general");
  alice->send({MsgType::Say, {"general", "\x1b[2J"}});
  auto reply = alice->expect(MsgType::Error);
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->field(0), "say");
}

TEST_F(ServerTest, ResumeReplaysMissedMessages) {
  auto alice = connect();
  std::string token = alice->sign_up("alice");
  alice->send({MsgType::Join, {"general"}});
  ASSERT_TRUE(alice->expect(MsgType::SyncDone).has_value());
  auto bob = member("bob", "general");
  bob->send({MsgType::Say, {"general", "first"}});
  ASSERT_TRUE(alice->expect(MsgType::RoomMsg).has_value());

  alice.reset();  // Connection drops.
  ASSERT_TRUE(bob->expect(MsgType::RoomEvent).has_value());
  bob->send({MsgType::Say, {"general", "second"}});
  bob->send({MsgType::Say, {"general", "third"}});

  auto again = connect();
  again->send({MsgType::Resume, {token}});
  auto welcome = again->expect(MsgType::Welcome);
  ASSERT_TRUE(welcome.has_value());
  EXPECT_EQ(welcome->field(0), "alice");
  again->send({MsgType::Join, {"general", "1"}});
  std::vector<std::string> replayed;
  for (;;) {
    auto m = again->receive();
    ASSERT_TRUE(m.has_value());
    if (m->type == MsgType::SyncDone) {
      EXPECT_EQ(m->field(1), "3");
      break;
    }
    if (m->type == MsgType::RoomMsg) replayed.push_back(m->field(4));
  }
  EXPECT_EQ(replayed, (std::vector<std::string>{"second", "third"}));
}

TEST_F(ServerTest, ResumeWithUnknownTokenFails) {
  auto client = connect();
  client->send({MsgType::Resume, {"bogus"}});
  auto reply = client->receive();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->type, MsgType::Error);
  EXPECT_EQ(reply->field(0), "resume");
}

TEST_F(ServerTest, SecondLoginKicksFirst) {
  auto first = connect();
  first->sign_up("alice");
  auto second = connect();
  second->send({MsgType::Login, {"alice", "password123"}});
  ASSERT_TRUE(second->expect(MsgType::Welcome).has_value());
  auto kicked = first->expect(MsgType::Error);
  ASSERT_TRUE(kicked.has_value());
  EXPECT_EQ(kicked->field(0), "session");
  EXPECT_TRUE(first->closed_by_server());
}

TEST_F(ServerTest, LogoutEndsSession) {
  auto client = connect();
  std::string token = client->sign_up("alice");
  client->send({MsgType::Logout, {}});
  ASSERT_TRUE(client->expect(MsgType::Ok).has_value());
  client->send({MsgType::Resume, {token}});
  auto reply = client->receive();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->type, MsgType::Error);
}

TEST_F(ServerTest, MalformedFrameClosesConnection) {
  auto client = connect();
  client->send_raw(std::string("\x00\x00\x00\x03\xee\x00\x00", 7));
  auto reply = client->receive();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->type, MsgType::Error);
  EXPECT_EQ(reply->field(0), "protocol");
  EXPECT_TRUE(client->closed_by_server());
}

TEST_F(ServerTest, PlainTcpIsRejected) {
  auto fd = net::tcp_connect("127.0.0.1", server_.port(), 5s);
  const char hello[] = "GET / HTTP/1.0\r\n\r\n";
  ASSERT_GT(::write(fd.get(), hello, sizeof hello - 1), 0);
  pollfd pfd{fd.get(), POLLIN, 0};
  ASSERT_EQ(::poll(&pfd, 1, 5000), 1);
  char buffer[512];
  // The server answers with a TLS alert at most, then closes.
  while (::read(fd.get(), buffer, sizeof buffer) > 0) {
  }
  SUCCEED();
}

TEST_F(ServerTest, PingEchoes) {
  auto client = connect();
  client->send({MsgType::Ping, {"42"}});
  auto pong = client->receive();
  ASSERT_TRUE(pong.has_value());
  EXPECT_EQ(pong->type, MsgType::Pong);
  EXPECT_EQ(pong->field(0), "42");
}

TEST_F(ServerTest, BroadcastReachesManyClients) {
  constexpr int kClients = 40;
  std::vector<std::unique_ptr<TestClient>> clients;
  for (int i = 0; i < kClients; ++i) clients.push_back(member("user" + std::to_string(i), "lobby"));
  clients[0]->send({MsgType::Say, {"lobby", "hello everyone"}});
  for (auto& client : clients) {
    auto got = client->expect(MsgType::RoomMsg);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->field(4), "hello everyone");
  }
  clients[0]->send({MsgType::Who, {"lobby"}});
  auto members = clients[0]->expect(MsgType::Members);
  ASSERT_TRUE(members.has_value());
  EXPECT_EQ(members->fields.size(), 1u + kClients);
}

}  // namespace
}  // namespace buzz::test
