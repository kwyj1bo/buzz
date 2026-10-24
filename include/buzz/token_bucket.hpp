#pragma once

#include <algorithm>
#include <chrono>

namespace buzz {

// Allows `rate` actions per second on average, with bursts up to `burst`.
class TokenBucket {
 public:
  using Clock = std::chrono::steady_clock;

  TokenBucket(double rate, double burst, Clock::time_point now = Clock::now())
      : rate_(rate), burst_(burst), tokens_(burst), last_(now) {}

  // Takes one token. Returns false when the caller is over the limit.
  bool take(Clock::time_point now = Clock::now()) {
    std::chrono::duration<double> elapsed = now - last_;
    last_ = now;
    tokens_ = std::min(burst_, tokens_ + elapsed.count() * rate_);
    if (tokens_ < 1.0) return false;
    tokens_ -= 1.0;
    return true;
  }

 private:
  double rate_;
  double burst_;
  double tokens_;
  Clock::time_point last_;
};

}  // namespace buzz
