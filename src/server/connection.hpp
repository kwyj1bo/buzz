#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "buzz/net.hpp"
#include "buzz/protocol.hpp"
#include "buzz/server.hpp"
#include "buzz/tls.hpp"
#include "buzz/token_bucket.hpp"

namespace buzz {

struct Server::Connection {
  Connection(double rate, double burst) : limiter(rate, burst) {}

  uint64_t id = 0;
  net::UniqueFd fd;
  tls::SslPtr ssl;
  std::string peer;
  bool handshake_done = false;
  bool handshake_wants_write = false;
  bool closing = false;
  bool auth_pending = false;
  bool ever_logged_in = false;
  int failed_logins = 0;
  std::chrono::steady_clock::time_point accepted_at = std::chrono::steady_clock::now();
  TokenBucket limiter;
  uint32_t interest = 0;
  FrameDecoder decoder;
  std::string out;
  std::string user;  // Empty until logged in.
};

}  // namespace buzz
