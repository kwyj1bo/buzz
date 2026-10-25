#include "buzz/room_manager.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace buzz {
namespace {

// Bounds the memory of seqs kept for evicted rooms.
constexpr size_t kMaxRetired = 65'536;

}  // namespace

RoomManager::RoomManager(size_t history_limit, size_t max_rooms, bool persistent,
                         size_t max_rooms_per_user)
    : history_limit_(history_limit),
      max_rooms_(max_rooms),
      persistent_(persistent),
      max_rooms_per_user_(max_rooms_per_user) {}

RoomManager::JoinResult RoomManager::join(const std::string& room, const std::string& user) {
  if (is_member(room, user)) return JoinResult::AlreadyMember;
  auto count = joined_.find(user);
  if (count != joined_.end() && count->second >= max_rooms_per_user_) {
    return JoinResult::UserRoomLimit;
  }
  Room* r = open(room);
  if (!r) return JoinResult::TooManyRooms;
  r->members.insert(user);
  r->last_active = ++clock_;
  ++joined_[user];
  return JoinResult::Joined;
}

bool RoomManager::leave(const std::string& room, const std::string& user) {
  auto it = rooms_.find(room);
  if (it == rooms_.end() || it->second.members.erase(user) == 0) return false;
  forget_membership(user);
  release_if_unused(it);
  return true;
}

std::vector<std::string> RoomManager::leave_all(const std::string& user) {
  std::vector<std::string> left;
  for (auto it = rooms_.begin(); it != rooms_.end();) {
    auto current = it++;
    if (current->second.members.erase(user) == 0) continue;
    left.push_back(current->first);
    release_if_unused(current);
  }
  joined_.erase(user);
  return left;
}

const StoredMessage& RoomManager::post(const std::string& room, const std::string& from,
                                       const std::string& text, int64_t timestamp_ms) {
  auto it = rooms_.find(room);
  if (it == rooms_.end()) throw std::logic_error("post to unknown room " + room);
  Room& r = it->second;
  r.last_active = ++clock_;
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

bool RoomManager::restore(const std::string& room, uint64_t last_seq,
                          std::vector<StoredMessage> recent) {
  Room* r = open(room);
  if (!r) return false;
  r->next_seq = std::max(r->next_seq, last_seq + 1);
  r->history.assign(std::make_move_iterator(recent.begin()), std::make_move_iterator(recent.end()));
  while (r->history.size() > history_limit_) r->history.pop_front();
  return true;
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

RoomManager::Room* RoomManager::open(const std::string& room) {
  auto it = rooms_.find(room);
  if (it != rooms_.end()) return &it->second;
  if (rooms_.size() >= max_rooms_ && !evict_idle_room()) return nullptr;
  Room& r = rooms_[room];
  if (auto retired = retired_seq_.find(room); retired != retired_seq_.end()) {
    r.next_seq = retired->second + 1;
    retired_seq_.erase(retired);
  }
  r.last_active = ++clock_;
  return &r;
}

bool RoomManager::evict_idle_room() {
  auto victim = rooms_.end();
  for (auto it = rooms_.begin(); it != rooms_.end(); ++it) {
    if (!it->second.members.empty()) continue;
    if (victim == rooms_.end() || it->second.last_active < victim->second.last_active) victim = it;
  }
  if (victim == rooms_.end()) return false;
  if (retired_seq_.size() >= kMaxRetired) retired_seq_.clear();
  retired_seq_[victim->first] = victim->second.next_seq - 1;
  rooms_.erase(victim);
  return true;
}

void RoomManager::release_if_unused(std::map<std::string, Room>::iterator it) {
  const Room& r = it->second;
  if (!r.members.empty()) return;
  // With storage the room reloads from disk; without it an empty room with
  // history stays until evicted, so its messages remain available.
  if (persistent_ || r.history.empty()) {
    if (!persistent_ && r.next_seq > 1) retired_seq_[it->first] = r.next_seq - 1;
    rooms_.erase(it);
  }
}

void RoomManager::forget_membership(const std::string& user) {
  auto it = joined_.find(user);
  if (it == joined_.end()) return;
  if (--it->second == 0) joined_.erase(it);
}

}  // namespace buzz
