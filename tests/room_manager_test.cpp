#include <gtest/gtest.h>

#include "buzz/room_manager.hpp"

namespace buzz {
namespace {

using Join = RoomManager::JoinResult;

TEST(RoomManager, JoinAndLeave) {
  RoomManager rooms(10, 10);
  EXPECT_EQ(rooms.join("general", "alice"), Join::Joined);
  EXPECT_EQ(rooms.join("general", "alice"), Join::AlreadyMember);
  EXPECT_EQ(rooms.join("general", "bob"), Join::Joined);
  EXPECT_EQ(rooms.members("general"), (std::vector<std::string>{"alice", "bob"}));
  EXPECT_TRUE(rooms.leave("general", "alice"));
  EXPECT_FALSE(rooms.leave("general", "alice"));
  EXPECT_FALSE(rooms.is_member("general", "alice"));
}

TEST(RoomManager, LeaveAllReportsRooms) {
  RoomManager rooms(10, 10);
  rooms.join("a", "alice");
  rooms.join("b", "alice");
  rooms.join("b", "bob");
  EXPECT_EQ(rooms.leave_all("alice"), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(rooms.members("b"), std::vector<std::string>{"bob"});
}

TEST(RoomManager, LimitsRoomCount) {
  RoomManager rooms(10, 2);
  EXPECT_EQ(rooms.join("a", "alice"), Join::Joined);
  EXPECT_EQ(rooms.join("b", "alice"), Join::Joined);
  EXPECT_EQ(rooms.join("c", "alice"), Join::TooManyRooms);
  EXPECT_EQ(rooms.join("a", "bob"), Join::Joined);
}

TEST(RoomManager, PostNumbersMessages) {
  RoomManager rooms(10, 10);
  rooms.join("general", "alice");
  EXPECT_EQ(rooms.post("general", "alice", "one", 1).seq, 1u);
  EXPECT_EQ(rooms.post("general", "alice", "two", 2).seq, 2u);
  EXPECT_EQ(rooms.last_seq("general"), 2u);
  EXPECT_EQ(rooms.last_seq("missing"), 0u);
}

TEST(RoomManager, HistorySinceReturnsOnlyNewer) {
  RoomManager rooms(10, 10);
  rooms.join("general", "alice");
  for (int i = 1; i <= 5; ++i) rooms.post("general", "alice", std::to_string(i), i);
  auto missed = rooms.history_since("general", 3, 100);
  ASSERT_EQ(missed.size(), 2u);
  EXPECT_EQ(missed[0].text, "4");
  EXPECT_EQ(missed[1].text, "5");
  EXPECT_TRUE(rooms.history_since("general", 5, 100).empty());
}

TEST(RoomManager, HistoryLimitKeepsNewest) {
  RoomManager rooms(10, 10);
  rooms.join("general", "alice");
  for (int i = 1; i <= 5; ++i) rooms.post("general", "alice", std::to_string(i), i);
  auto recent = rooms.history_since("general", 0, 2);
  ASSERT_EQ(recent.size(), 2u);
  EXPECT_EQ(recent[0].seq, 4u);
  EXPECT_EQ(recent[1].seq, 5u);
}

TEST(RoomManager, HistoryIsBounded) {
  RoomManager rooms(3, 10);
  rooms.join("general", "alice");
  for (int i = 1; i <= 10; ++i) rooms.post("general", "alice", std::to_string(i), i);
  auto all = rooms.history_since("general", 0, 100);
  ASSERT_EQ(all.size(), 3u);
  EXPECT_EQ(all.front().seq, 8u);
  EXPECT_EQ(rooms.last_seq("general"), 10u);
}

}  // namespace
}  // namespace buzz
