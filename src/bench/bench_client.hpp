#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "buzz/net.hpp"
#include "buzz/protocol.hpp"
#include "buzz/tls.hpp"

namespace buzz::bench {

// One load-generating connection. Setup (connect, login, join) is blocking;
// the measured phase switches it to non-blocking for an epoll loop.
class BenchClient {
 public:
  BenchClient(SSL_CTX* ctx, uint16_t port);

  // Wall time of TCP connect plus TLS handshake, in microseconds.
  double handshake_micros() const { return handshake_micros_; }
  int fd() const { return fd_.get(); }
  const std::string& name() const { return name_; }

  // Blocking helpers for setup.
  void send(const Message& message);
  std::optional<Message> expect(MsgType type, std::chrono::milliseconds timeout);
  void sign_up(const std::string& user, const std::string& password);
  void login(const std::string& user, const std::string& password);
  void join(const std::string& room);

  // Non-blocking phase.
  void make_nonblocking();
  void queue(const Message& message);
  // Writes what it can. Returns true while output is still pending.
  bool flush();
  // Reads what is available and decodes messages. Returns false on close.
  bool read_available(std::vector<Message>& out);

 private:
  net::UniqueFd fd_;
  tls::SslPtr ssl_;
  FrameDecoder decoder_;
  std::string out_;
  std::string name_;
  double handshake_micros_ = 0;
};

}  // namespace buzz::bench
