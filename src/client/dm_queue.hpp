#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace buzz::client {

// Direct messages waiting for the server to introduce a peer. Every entry
// either has an introduction request in flight or is re-requested after a
// reconnect, and a request that gets no answer in time hands its messages
// back for the relay. So no message can wait forever.
class DmQueue {
 public:
  using Clock = std::chrono::steady_clock;
  using Expired = std::vector<std::pair<std::string, std::vector<std::string>>>;

  explicit DmQueue(Clock::duration timeout = std::chrono::seconds(5)) : timeout_(timeout) {}

  // Queues a message. Returns true when the caller should send an
  // introduction request now. Pass can_request = false while offline.
  bool add(const std::string& peer, std::string text, bool can_request, Clock::time_point now);
  // Removes and returns a peer's messages.
  std::vector<std::string> take(const std::string& peer);
  // Requests in flight on a dropped link will never be answered.
  void on_disconnect();
  // Peers with messages but no request in flight; they are marked requested.
  std::vector<std::string> request_again(Clock::time_point now);
  // Removes and returns peers whose request was not answered in time.
  Expired take_expired(Clock::time_point now);
  bool waiting(const std::string& peer) const { return entries_.count(peer) != 0; }

 private:
  struct Entry {
    std::vector<std::string> texts;
    std::optional<Clock::time_point> requested_at;
  };

  Clock::duration timeout_;
  std::map<std::string, Entry> entries_;
};

}  // namespace buzz::client
