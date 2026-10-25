#include "dm_queue.hpp"

namespace buzz::client {

bool DmQueue::add(const std::string& peer, std::string text, bool can_request,
                  Clock::time_point now) {
  Entry& entry = entries_[peer];
  entry.texts.push_back(std::move(text));
  if (entry.requested_at || !can_request) return false;
  entry.requested_at = now;
  return true;
}

std::vector<std::string> DmQueue::take(const std::string& peer) {
  auto it = entries_.find(peer);
  if (it == entries_.end()) return {};
  std::vector<std::string> texts = std::move(it->second.texts);
  entries_.erase(it);
  return texts;
}

void DmQueue::on_disconnect() {
  for (auto& [peer, entry] : entries_) entry.requested_at.reset();
}

std::vector<std::string> DmQueue::request_again(Clock::time_point now) {
  std::vector<std::string> peers;
  for (auto& [peer, entry] : entries_) {
    if (entry.requested_at) continue;
    entry.requested_at = now;
    peers.push_back(peer);
  }
  return peers;
}

DmQueue::Expired DmQueue::take_expired(Clock::time_point now) {
  Expired expired;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.requested_at && now - *it->second.requested_at >= timeout_) {
      expired.emplace_back(it->first, std::move(it->second.texts));
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  return expired;
}

}  // namespace buzz::client
