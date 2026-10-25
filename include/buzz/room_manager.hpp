#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace buzz {

struct StoredMessage {
  uint64_t seq = 0;
  int64_t timestamp_ms = 0;
  std::string from;
  std::string text;
};

// Chatrooms with members and a bounded message history.
// Each room numbers its messages from 1, so a client that knows the last
// sequence it saw can ask for exactly what it missed.
//
// Rooms are freed so that nobody can use up the room limit for good:
//  - a room with no members and no history is removed at once;
//  - with persistent storage, any room is removed when its last member
//    leaves (it reloads from disk on the next join);
//  - otherwise an empty room is evicted, least recently active first, when
//    the limit is reached. Its last seq is kept so numbering never restarts.
// Each user may be in at most max_rooms_per_user rooms.
// Not thread-safe: owned by the server event loop.
class RoomManager {
 public:
  enum class JoinResult { Joined, AlreadyMember, TooManyRooms, UserRoomLimit };

  RoomManager(size_t history_limit, size_t max_rooms, bool persistent = false,
              size_t max_rooms_per_user = 64);

  JoinResult join(const std::string& room, const std::string& user);
  bool leave(const std::string& room, const std::string& user);
  // Returns the rooms the user left.
  std::vector<std::string> leave_all(const std::string& user);

  const StoredMessage& post(const std::string& room, const std::string& from,
                            const std::string& text, int64_t timestamp_ms);
  // Messages with seq > since, at most `limit`, newest kept, oldest first.
  std::vector<StoredMessage> history_since(const std::string& room, uint64_t since,
                                           size_t limit) const;

  // Loads a room from storage. Returns false when the room limit is full.
  bool restore(const std::string& room, uint64_t last_seq, std::vector<StoredMessage> recent);
  // Seq of the oldest message still in memory, or 0 when there is none.
  uint64_t oldest_seq(const std::string& room) const;

  bool exists(const std::string& room) const { return rooms_.count(room) != 0; }
  bool is_member(const std::string& room, const std::string& user) const;
  std::vector<std::string> members(const std::string& room) const;
  std::vector<std::string> rooms() const;
  uint64_t last_seq(const std::string& room) const;
  size_t size() const { return rooms_.size(); }

 private:
  struct Room {
    std::set<std::string> members;
    std::deque<StoredMessage> history;
    uint64_t next_seq = 1;
    uint64_t last_active = 0;
  };

  const Room* find(const std::string& room) const;
  // Finds or creates a room, evicting an idle one if needed. Null when full.
  Room* open(const std::string& room);
  bool evict_idle_room();
  void release_if_unused(std::map<std::string, Room>::iterator it);
  void forget_membership(const std::string& user);

  size_t history_limit_;
  size_t max_rooms_;
  bool persistent_;
  size_t max_rooms_per_user_;
  uint64_t clock_ = 0;  // Activity counter for eviction order.
  std::map<std::string, Room> rooms_;
  std::unordered_map<std::string, uint64_t> retired_seq_;  // Evicted room to last seq.
  std::unordered_map<std::string, size_t> joined_;         // User to room count.
};

}  // namespace buzz
