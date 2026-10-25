#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "buzz/net.hpp"
#include "buzz/protocol.hpp"
#include "buzz/tls.hpp"

namespace buzz::test {

using namespace std::chrono_literals;

// Blocking TLS client for driving the server in tests. It verifies the
// server certificate against the test CA.
class TestClient {
 public:
  TestClient(const std::string& ca_file, uint16_t port);

  void send(const Message& message);
  void send_raw(const std::string& bytes);
  // Next message, or nullopt on timeout or close.
  std::optional<Message> receive(std::chrono::milliseconds timeout = 5s);
  // Skips messages until one of the given type arrives.
  std::optional<Message> expect(MsgType type, std::chrono::milliseconds timeout = 5s);
  // True when the server closes the connection within the timeout.
  bool closed_by_server(std::chrono::milliseconds timeout = 5s);

  // Registers, logs in and returns the session token.
  std::string sign_up(const std::string& user, const std::string& password = "password123");
  // The last Welcome seen by sign_up.
  const Message& welcome() const { return welcome_; }

 private:
  enum class ReadResult { Data, Timeout, Closed };
  ReadResult read_some(std::chrono::milliseconds timeout);

  tls::ContextPtr ctx_;
  net::UniqueFd fd_;
  tls::SslPtr ssl_;
  FrameDecoder decoder_;
  Message welcome_;
};

}  // namespace buzz::test
