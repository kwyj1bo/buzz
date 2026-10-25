#include "support/test_server.hpp"

#include <gtest/gtest.h>
#include <stdlib.h>

#include <filesystem>

#include "buzz/log.hpp"
#include "buzz/tls.hpp"

namespace buzz::test {

TestServer::TestServer(const std::string& db_path, const Tweak& tweak) {
  std::string pattern = ::testing::TempDir() + "buzz-XXXXXX";
  dir_ = ::mkdtemp(pattern.data());
  cert_file_ = dir_ + "/server.crt";
  std::string key_file = dir_ + "/server.key";
  tls::generate_self_signed(cert_file_, key_file);

  log::set_level(log::Level::Warn);
  ServerConfig config;
  config.port = 0;
  config.udp_port = 0;
  config.cert_file = cert_file_;
  config.key_file = key_file;
  config.db_path = db_path;
  config.message_rate = 1e9;
  config.message_burst = 1e9;
  config.auth.per_minute = 1e12;
  config.auth.burst = 1e9;
  if (tweak) tweak(config);
  config.pbkdf2_iterations = 1000;
  config.worker_threads = 2;
  server_ = std::make_unique<Server>(config);
  server_->start();
  thread_ = std::thread([this] { server_->run(); });
}

TestServer::~TestServer() {
  server_->stop();
  thread_.join();
  server_.reset();
  std::filesystem::remove_all(dir_);
}

}  // namespace buzz::test
