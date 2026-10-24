// Restarts a real server on the same SQLite file and checks that accounts
// and room history survive.
#include <gtest/gtest.h>
#include <stdlib.h>

#include <filesystem>
#include <memory>

#include "support/test_client.hpp"
#include "support/test_server.hpp"

namespace buzz::test {
namespace {

class PersistenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern = ::testing::TempDir() + "buzz-persist-XXXXXX";
    dir_ = ::mkdtemp(pattern.data());
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string db() const { return dir_ + "/buzz.db"; }

  std::string dir_;
};

TEST_F(PersistenceTest, AccountsAndHistorySurviveRestart) {
  {
    TestServer server(db());
    TestClient alice(server.cert_file(), server.port());
    alice.sign_up("alice");
    alice.send({MsgType::Join, {"general"}});
    ASSERT_TRUE(alice.expect(MsgType::SyncDone).has_value());
    alice.send({MsgType::Say, {"general", "before restart"}});
    ASSERT_TRUE(alice.expect(MsgType::RoomMsg).has_value());
  }

  TestServer server(db());
  TestClient alice(server.cert_file(), server.port());
  alice.send({MsgType::Login, {"alice", "password123"}});
  ASSERT_TRUE(alice.expect(MsgType::Welcome).has_value());
  alice.send({MsgType::Join, {"general"}});
  auto old = alice.expect(MsgType::RoomMsg);
  ASSERT_TRUE(old.has_value());
  EXPECT_EQ(old->field(1), "1");
  EXPECT_EQ(old->field(4), "before restart");

  alice.send({MsgType::Say, {"general", "after restart"}});
  auto fresh = alice.expect(MsgType::RoomMsg);
  ASSERT_TRUE(fresh.has_value());
  EXPECT_EQ(fresh->field(1), "2");  // Numbering continues.
}

TEST_F(PersistenceTest, OldGapsAreReadFromDisk) {
  {
    TestServer server(db());
    TestClient alice(server.cert_file(), server.port());
    alice.sign_up("alice");
    alice.send({MsgType::Join, {"general"}});
    ASSERT_TRUE(alice.expect(MsgType::SyncDone).has_value());
    // More than history_limit (256), so the oldest leave memory.
    for (int i = 1; i <= 300; ++i) alice.send({MsgType::Say, {"general", std::to_string(i)}});
    for (int i = 1; i <= 300; ++i) ASSERT_TRUE(alice.expect(MsgType::RoomMsg).has_value());
  }
  TestServer server(db());
  TestClient alice(server.cert_file(), server.port());
  alice.send({MsgType::Login, {"alice", "password123"}});
  ASSERT_TRUE(alice.expect(MsgType::Welcome).has_value());
  alice.send({MsgType::Join, {"general", "10"}});
  auto first = alice.expect(MsgType::RoomMsg);
  ASSERT_TRUE(first.has_value());
  // Memory holds only 45..300 after the restart; 11..44 come from disk.
  EXPECT_EQ(first->field(1), "11");
}

}  // namespace
}  // namespace buzz::test
