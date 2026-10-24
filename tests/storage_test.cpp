#include <gtest/gtest.h>
#include <stdlib.h>

#include <filesystem>

#include "buzz/storage.hpp"
#include "buzz/user_store.hpp"

namespace buzz {
namespace {

class StorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern = ::testing::TempDir() + "buzz-db-XXXXXX";
    dir_ = ::mkdtemp(pattern.data());
    path_ = dir_ + "/buzz.db";
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  StoredMessage message(uint64_t seq) {
    return {seq, static_cast<int64_t>(seq) * 10, "alice", "m" + std::to_string(seq)};
  }

  std::string dir_;
  std::string path_;
};

TEST_F(StorageTest, UsersSurviveReopen) {
  {
    Storage storage(path_);
    EXPECT_TRUE(storage.insert_user("alice", crypto::hash_password("password123", 1000)));
    EXPECT_FALSE(storage.insert_user("alice", crypto::hash_password("other-pass", 1000)));
  }
  Storage storage(path_);
  auto users = storage.load_users();
  ASSERT_EQ(users.size(), 1u);
  EXPECT_EQ(users[0].first, "alice");
  EXPECT_TRUE(crypto::verify_password("password123", users[0].second));
}

TEST_F(StorageTest, MessagesAreWrittenOnFlush) {
  Storage storage(path_);
  for (uint64_t seq = 1; seq <= 3; ++seq) storage.append_message("general", message(seq));
  EXPECT_EQ(storage.flush(), 3u);
  EXPECT_EQ(storage.flush(), 0u);
}

TEST_F(StorageTest, LoadRoomsReturnsRecentHistory) {
  {
    Storage storage(path_);
    for (uint64_t seq = 1; seq <= 10; ++seq) storage.append_message("general", message(seq));
    storage.append_message("random", message(1));
  }  // Destructor flushes.
  Storage storage(path_);
  auto rooms = storage.load_rooms(3);
  ASSERT_EQ(rooms.size(), 2u);
  EXPECT_EQ(rooms[0].room, "general");
  EXPECT_EQ(rooms[0].last_seq, 10u);
  ASSERT_EQ(rooms[0].recent.size(), 3u);
  EXPECT_EQ(rooms[0].recent.front().seq, 8u);
  EXPECT_EQ(rooms[0].recent.back().text, "m10");
  EXPECT_EQ(rooms[1].room, "random");
  EXPECT_EQ(rooms[1].recent.size(), 1u);
}

TEST_F(StorageTest, HistorySinceIncludesQueuedMessages) {
  Storage storage(path_);
  for (uint64_t seq = 1; seq <= 5; ++seq) storage.append_message("general", message(seq));
  auto missed = storage.history_since("general", 2, 100);
  ASSERT_EQ(missed.size(), 3u);
  EXPECT_EQ(missed.front().seq, 3u);
  EXPECT_EQ(missed.back().from, "alice");
  EXPECT_EQ(storage.history_since("general", 0, 2).front().seq, 4u);
}

TEST_F(StorageTest, UserStoreWritesThrough) {
  {
    Storage storage(path_);
    UserStore users(1000, &storage);
    EXPECT_EQ(users.add_user("alice", "password123"), UserStore::RegisterResult::Ok);
  }
  Storage storage(path_);
  UserStore users(1000, &storage);
  EXPECT_EQ(users.size(), 1u);
  EXPECT_TRUE(users.authenticate("alice", "password123"));
  EXPECT_EQ(users.add_user("alice", "password123"), UserStore::RegisterResult::Exists);
}

TEST_F(StorageTest, BadPathThrows) {
  EXPECT_THROW(Storage(dir_ + "/missing/dir/buzz.db"), StorageError);
}

}  // namespace
}  // namespace buzz
