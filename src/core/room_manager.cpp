#include "buzz/room_manager.hpp"

#include <iterator>
#include <stdexcept>

namespace buzz {

RoomManager::RoomManager(size_t history_limit, size_t max_rooms)
    : history_limit_(history_limit), max_rooms_(max_rooms) {}

RoomManager::JoinResult RoomManager::join(const std::string& room, const std::string& user) {
  auto it = rooms_.find(room);
  if (it == rooms_.end()) {
    if (rooms_.size() >= max_rooms_) return JoinResult::TooManyRooms;
    it = rooms_.emplace(room, Room{}).first;
  }
  return it->second.members.insert(user).second ? JoinResult::Joined : JoinResult::AlreadyMember;
}

bool RoomManager::leave(const std::string& room, const std::string& user) {
  auto it = rooms_.find(room);
  return it != rooms_.end() && it->second.members.erase(user) > 0;
}

std::vector<std::string> RoomManager::leave_all(const std::string& user) {
  std::vector<std::string> left;
  for (auto& [name, room] : rooms_) {
    if (room.members.erase(user) > 0) left.push_back(name);
  }
  return left;
}

const StoredMessage& RoomManager::post(const std::string& room, const std::string& from,
                                       const std::string& text, int64_t timestamp_ms) {
  auto it = rooms_.find(room);
  if (it == rooms_.end()) throw std::logic_error("post to unknown room " + room);
  Room& r = it->second;
  r.history.push_back({r.next_seq++, timestamp_ms, from, text});
  while (r.history.size() > history_limit_) r.history.pop_front();
  return r.history.back();
}

std::vector<StoredMessage> RoomManager::history_since(const std::string& room, uint64_t since,
                                                      size_t limit) const {
  const Room* r = find(room);
  if (!r || limit == 0) return {};
  // Walk back from the newest message; history is ordered by seq.
  auto first = r->history.end();
  size_t count = 0;
  while (first != r->history.begin() && count < limit) {
    auto prev = std::prev(first);
    if (prev->seq <= since) break;
    first = prev;
    ++count;
  }
  return {first, r->history.end()};
}

void RoomManager::restore(const std::string& room, uint64_t last_seq,
                          std::vector<StoredMessage> recent) {
  Room& r = rooms_[room];
  r.next_seq = last_seq + 1;
  r.history.assign(std::make_move_iterator(recent.begin()), std::make_move_iterator(recent.end()));
  while (r.history.size() > history_limit_) r.history.pop_front();
}

uint64_t RoomManager::oldest_seq(const std::string& room) const {
  const Room* r = find(room);
  return r && !r->history.empty() ? r->history.front().seq : 0;
}

bool RoomManager::is_member(const std::string& room, const std::string& user) const {
  const Room* r = find(room);
  return r && r->members.count(user) != 0;
}

std::vector<std::string> RoomManager::members(const std::string& room) const {
  const Room* r = find(room);
  if (!r) return {};
  return {r->members.begin(), r->members.end()};
}

std::vector<std::string> RoomManager::rooms() const {
  std::vector<std::string> names;
  names.reserve(rooms_.size());
  for (const auto& entry : rooms_) names.push_back(entry.first);
  return names;
}

uint64_t RoomManager::last_seq(const std::string& room) const {
  const Room* r = find(room);
  return r ? r->next_seq - 1 : 0;
}

const RoomManager::Room* RoomManager::find(const std::string& room) const {
  auto it = rooms_.find(room);
  return it == rooms_.end() ? nullptr : &it->second;
}

}  // namespace buzz
