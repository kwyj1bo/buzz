#include <gtest/gtest.h>
#include <stdlib.h>

#include <filesystem>

#include <sqlite3.h>

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

TEST_F(StorageTest, LoadRoomReturnsRecentHistory) {
  {
    Storage storage(path_);
    for (uint64_t seq = 1; seq <= 10; ++seq) storage.append_message("general", message(seq));
  }  // Destructor flushes.
  Storage storage(path_);
  auto room = storage.load_room("general", 3);
  ASSERT_TRUE(room.has_value());
  EXPECT_EQ(room->last_seq, 10u);
  ASSERT_EQ(room->recent.size(), 3u);
  EXPECT_EQ(room->recent.front().seq, 8u);
  EXPECT_EQ(room->recent.back().text, "m10");
  EXPECT_FALSE(storage.load_room("empty", 3).has_value());
}

TEST_F(StorageTest, SessionsRoundTrip) {
  Storage storage(path_);
  storage.save_session("alice", "hash-a");
  storage.save_session("alice", "hash-b");  // Replaces.
  storage.save_session("bob", "hash-c");
  storage.delete_session("bob");
  auto sessions = storage.load_sessions();
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_EQ(sessions[0].first, "alice");
  EXPECT_EQ(sessions[0].second, "hash-b");
}

// Bug 8: when writes fail, messages stay queued, bounded, instead of
// growing without limit.
TEST_F(StorageTest, FailedFlushKeepsBoundedQueue) {
  Storage storage(path_, /*max_queued=*/3);
  {
    sqlite3* other = nullptr;
    ASSERT_EQ(sqlite3_open(path_.c_str(), &other), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(other, "DROP TABLE messages", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(other);
  }
  for (uint64_t seq = 1; seq <= 5; ++seq) storage.append_message("general", message(seq));
  EXPECT_THROW(storage.flush(), StorageError);
  EXPECT_EQ(storage.pending(), 3u);
  EXPECT_EQ(storage.dropped(), 2u);
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
