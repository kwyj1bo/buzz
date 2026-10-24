#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "buzz/server.hpp"

namespace buzz::test {

// Runs a real server on ephemeral ports in a background thread, with a
// fresh self-signed certificate and cheap password hashing.
class TestServer {
 public:
  using Tweak = std::function<void(ServerConfig&)>;

  // db_path empty keeps everything in memory. Rate limits are off unless a
  // tweak sets them.
  explicit TestServer(const std::string& db_path = "", const Tweak& tweak = {});
  ~TestServer();

  uint16_t port() const { return server_->port(); }
  uint16_t udp_port() const { return server_->udp_port(); }
  const std::string& cert_file() const { return cert_file_; }

 private:
  std::string dir_;
  std::string cert_file_;
  std::unique_ptr<Server> server_;
  std::thread thread_;
};

}  // namespace buzz::test
