#include "room_state.hpp"

namespace buzz::client {

bool RoomState::on_joined(const std::string& room) {
  rooms_.try_emplace(room);
  if (room != pending_join_ && !current_.empty()) return false;
  if (room == pending_join_) pending_join_.clear();
  current_ = room;
  return true;
}

void RoomState::on_left(const std::string& room) {
  rooms_.erase(room);
  if (current_ == room) current_ = rooms_.empty() ? "" : rooms_.begin()->first;
}

bool RoomState::accept(const std::string& room, uint64_t seq) {
  auto it = rooms_.find(room);
  return it != rooms_.end() && it->second.accept(seq);
}

void RoomState::on_synced(const std::string& room, uint64_t last_seq) {
  auto it = rooms_.find(room);
  if (it != rooms_.end()) it->second.advance_to(last_seq);
}

std::vector<std::pair<std::string, uint64_t>> RoomState::resume_points() const {
  std::vector<std::pair<std::string, uint64_t>> points;
  for (const auto& [room, window] : rooms_) points.emplace_back(room, window.contiguous());
  return points;
}

bool RoomState::set_current(const std::string& room) {
  if (!joined(room)) return false;
  current_ = room;
  return true;
}

void RoomState::clear() {
  rooms_.clear();
  current_.clear();
  pending_join_.clear();
}

}  // namespace buzz::client
