#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace buzz {

// Sender half of the reliable layer over UDP: numbers each payload and
// retransmits it with exponential backoff until it is acknowledged.
class ReliableSender {
 public:
  using Clock = std::chrono::steady_clock;

  struct Packet {
    uint64_t seq;
    std::string payload;
  };

  explicit ReliableSender(std::chrono::milliseconds rto = std::chrono::milliseconds(300),
                          int max_attempts = 6);

  // Queues a payload. It is due at once. Returns its sequence number.
  uint64_t enqueue(std::string payload, Clock::time_point now);
  // Packets to send now, first sends and retransmits.
  std::vector<Packet> due(Clock::time_point now);
  // Returns false for an unknown or repeated ack.
  bool ack(uint64_t seq);
  // Payloads dropped after max_attempts sends without an ack.
  std::vector<Packet> take_failed();
  // Removes and returns every unacknowledged payload, oldest first.
  std::vector<Packet> take_all();
  size_t in_flight() const { return pending_.size(); }

 private:
  struct Pending {
    std::string payload;
    int attempts = 0;
    Clock::time_point next_send;
  };

  std::chrono::milliseconds rto_;
  int max_attempts_;
  uint64_t next_seq_ = 1;
  std::map<uint64_t, Pending> pending_;
  std::vector<Packet> failed_;
};

// Receiver half: accepts each sequence number once, in any order.
class ReceiveWindow {
 public:
  // True the first time seq is seen.
  bool accept(uint64_t seq);
  // Marks every seq up to and including `seq` as seen.
  void advance_to(uint64_t seq);
  uint64_t contiguous() const { return contiguous_; }

 private:
  uint64_t contiguous_ = 0;     // All seqs <= this are seen.
  std::set<uint64_t> ahead_;    // Seen seqs above contiguous_.
};

}  // namespace buzz
