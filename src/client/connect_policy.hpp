#pragma once

#include <algorithm>
#include <chrono>

namespace buzz::client {

// Decides when the client (re)connects. With a session it reconnects on its
// own, with exponential backoff, so it can resume. Without one (never logged
// in, or logged out) it connects only when the user runs a command, so an
// idle prompt does not cycle through connections the server times out.
class ConnectPolicy {
 public:
  using Clock = std::chrono::steady_clock;

  bool should_connect(bool has_session, Clock::time_point now) const {
    return (has_session || wanted_) && now >= next_attempt_;
  }
  // The user ran a command that needs the server.
  void want_now() {
    wanted_ = true;
    next_attempt_ = {};
  }
  void on_connected() {
    backoff_ = std::chrono::seconds(1);
    wanted_ = false;
  }
  void on_failed(Clock::time_point now) {
    next_attempt_ = now + backoff_;
    backoff_ = std::min(backoff_ * 2, std::chrono::seconds(30));
  }
  void on_disconnected(Clock::time_point now) { next_attempt_ = now + backoff_; }
  std::chrono::seconds backoff() const { return backoff_; }

 private:
  bool wanted_ = true;  // Connect once at startup.
  Clock::time_point next_attempt_{};
  std::chrono::seconds backoff_{1};
};

}  // namespace buzz::client
