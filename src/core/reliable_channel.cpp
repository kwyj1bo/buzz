#include "buzz/reliable_channel.hpp"

namespace buzz {
namespace {

constexpr size_t kMaxAhead = 4096;

}  // namespace

ReliableSender::ReliableSender(std::chrono::milliseconds rto, int max_attempts)
    : rto_(rto), max_attempts_(max_attempts) {}

uint64_t ReliableSender::enqueue(std::string payload, Clock::time_point now) {
  uint64_t seq = next_seq_++;
  pending_.emplace(seq, Pending{std::move(payload), 0, now});
  return seq;
}

std::vector<ReliableSender::Packet> ReliableSender::due(Clock::time_point now) {
  std::vector<Packet> out;
  for (auto it = pending_.begin(); it != pending_.end();) {
    Pending& p = it->second;
    if (p.next_send > now) {
      ++it;
      continue;
    }
    if (p.attempts >= max_attempts_) {
      failed_.push_back({it->first, std::move(p.payload)});
      it = pending_.erase(it);
      continue;
    }
    out.push_back({it->first, p.payload});
    p.next_send = now + rto_ * (1 << p.attempts);
    ++p.attempts;
    ++it;
  }
  return out;
}

bool ReliableSender::ack(uint64_t seq) { return pending_.erase(seq) > 0; }

std::vector<ReliableSender::Packet> ReliableSender::take_failed() {
  std::vector<Packet> out;
  out.swap(failed_);
  return out;
}

std::vector<ReliableSender::Packet> ReliableSender::take_all() {
  std::vector<Packet> out;
  for (auto& [seq, pending] : pending_) out.push_back({seq, std::move(pending.payload)});
  pending_.clear();
  return out;
}

bool ReceiveWindow::accept(uint64_t seq) {
  if (seq == 0 || seq <= contiguous_) return false;
  if (!ahead_.insert(seq).second) return false;
  // A lost packet the sender gave up on leaves a permanent gap. Slide past
  // it so the set stays bounded.
  while (ahead_.size() > kMaxAhead) {
    contiguous_ = *ahead_.begin();
    ahead_.erase(ahead_.begin());
  }
  while (!ahead_.empty() && *ahead_.begin() == contiguous_ + 1) {
    ahead_.erase(ahead_.begin());
    ++contiguous_;
  }
  return true;
}

void ReceiveWindow::advance_to(uint64_t seq) {
  if (seq <= contiguous_) return;
  contiguous_ = seq;
  while (!ahead_.empty() && *ahead_.begin() <= contiguous_) ahead_.erase(ahead_.begin());
  while (!ahead_.empty() && *ahead_.begin() == contiguous_ + 1) {
    ahead_.erase(ahead_.begin());
    ++contiguous_;
  }
}

}  // namespace buzz
