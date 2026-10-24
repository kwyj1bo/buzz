#pragma once

#include <cstdint>
#include <string>

#include "buzz/net.hpp"
#include "buzz/protocol.hpp"
#include "buzz/server.hpp"
#include "buzz/tls.hpp"

namespace buzz {

struct Server::Connection {
  uint64_t id = 0;
  net::UniqueFd fd;
  tls::SslPtr ssl;
  std::string peer;
  bool handshake_done = false;
  bool handshake_wants_write = false;
  bool closing = false;
  bool auth_pending = false;
  uint32_t interest = 0;
  FrameDecoder decoder;
  std::string out;
  std::string user;  // Empty until logged in.
};

}  // namespace buzz
