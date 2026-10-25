#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "buzz/reliable_channel.hpp"

namespace buzz::client {

// The client's view of its rooms: which are joined, which is current, and
// which messages it has shown. Replayed history can arrive after newer live
// messages, so each room tracks seen seqs in a window, not a single maximum.
class RoomState {
 public:
  // Remembers a /join so the room becomes current only once the server
  // confirms it.
  void request_join(const std::string& room) { pending_join_ = room; }
  // Returns true when the room became the current room.
  bool on_joined(const std::string& room);
  void on_left(const std::string& room);
  // True the first time a message seq is seen in a joined room.
  bool accept(const std::string& room, uint64_t seq);
  // Everything up to last_seq has now been delivered or skipped.
  void on_synced(const std::string& room, uint64_t last_seq);
  // Joined rooms with the seq to resume each from.
  std::vector<std::pair<std::string, uint64_t>> resume_points() const;

  bool joined(const std::string& room) const { return rooms_.count(room) != 0; }
  const std::string& current() const { return current_; }
  bool set_current(const std::string& room);
  void clear();

 private:
  std::map<std::string, ReceiveWindow> rooms_;
  std::string current_;
  std::string pending_join_;
};

}  // namespace buzz::client
