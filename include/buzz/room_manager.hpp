#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
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
// Not thread-safe: owned by the server event loop.
class RoomManager {
 public:
  enum class JoinResult { Joined, AlreadyMember, TooManyRooms };

  RoomManager(size_t history_limit, size_t max_rooms);

  JoinResult join(const std::string& room, const std::string& user);
  bool leave(const std::string& room, const std::string& user);
  // Returns the rooms the user left.
  std::vector<std::string> leave_all(const std::string& user);

  const StoredMessage& post(const std::string& room, const std::string& from,
                            const std::string& text, int64_t timestamp_ms);
  // Messages with seq > since, at most `limit`, newest kept, oldest first.
  std::vector<StoredMessage> history_since(const std::string& room, uint64_t since,
                                           size_t limit) const;

  bool is_member(const std::string& room, const std::string& user) const;
  std::vector<std::string> members(const std::string& room) const;
  std::vector<std::string> rooms() const;
  uint64_t last_seq(const std::string& room) const;

 private:
  struct Room {
    std::set<std::string> members;
    std::deque<StoredMessage> history;
    uint64_t next_seq = 1;
  };

  const Room* find(const std::string& room) const;

  size_t history_limit_;
  size_t max_rooms_;
  std::map<std::string, Room> rooms_;
};

}  // namespace buzz
