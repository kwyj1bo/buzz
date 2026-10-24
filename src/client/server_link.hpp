#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "buzz/net.hpp"
#include "buzz/protocol.hpp"
#include "buzz/tls.hpp"

namespace buzz::client {

// TLS connection from the client to the server. Connects with blocking I/O,
// then switches to non-blocking so the client loop can poll it.
class ServerLink {
 public:
  ServerLink(tls::ContextPtr ctx, std::string host, uint16_t port);

  // Throws on a TCP or TLS failure, including a bad certificate.
  void connect(std::chrono::milliseconds timeout);
  void close();
  void send(const Message& message);
  // Reads what is available. Closes the link on EOF or a bad frame.
  std::vector<Message> receive();
  void flush();

  bool connected() const { return ssl_ != nullptr; }
  bool wants_write() const { return !out_.empty(); }
  int fd() const { return fd_.get(); }
  const net::Endpoint& remote() const { return remote_; }

 private:
  void handshake();

  tls::ContextPtr ctx_;
  std::string host_;
  uint16_t port_;
  net::UniqueFd fd_;
  tls::SslPtr ssl_;
  FrameDecoder decoder_;
  std::string out_;
  net::Endpoint remote_;
};

}  // namespace buzz::client
