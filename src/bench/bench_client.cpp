#include "bench_client.hpp"

#include <openssl/err.h>
#include <poll.h>

#include <algorithm>
#include <climits>
#include <stdexcept>

#include "stats.hpp"

namespace buzz::bench {
namespace {

bool would_block(int error) { return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE; }

}  // namespace

BenchClient::BenchClient(SSL_CTX* ctx, uint16_t port) {
  auto start = Clock::now();
  fd_ = net::tcp_connect("127.0.0.1", port, std::chrono::seconds(10));
  ssl_ = tls::make_client_ssl(ctx, fd_.get(), "127.0.0.1");
  if (SSL_connect(ssl_.get()) != 1) throw std::runtime_error("handshake: " + tls::last_error());
  handshake_micros_ = seconds_since(start) * 1e6;
}

void BenchClient::send(const Message& message) {
  std::string frame = encode_frame(message);
  if (SSL_write(ssl_.get(), frame.data(), static_cast<int>(frame.size())) <= 0) {
    throw std::runtime_error("write failed");
  }
}

std::optional<Message> BenchClient::expect(MsgType type, std::chrono::milliseconds timeout) {
  auto deadline = Clock::now() + timeout;
  char buffer[16 * 1024];
  for (;;) {
    while (auto message = decoder_.next()) {
      if (message->type == type) return message;
      if (message->type == MsgType::Error) {
        throw std::runtime_error("server error: " + message->field(0) + ": " + message->field(1));
      }
    }
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (left.count() <= 0) return std::nullopt;
    if (SSL_pending(ssl_.get()) == 0) {
      pollfd pfd{fd_.get(), POLLIN, 0};
      if (::poll(&pfd, 1, static_cast<int>(left.count())) <= 0) return std::nullopt;
    }
    int n = SSL_read(ssl_.get(), buffer, sizeof buffer);
    if (n > 0) {
      decoder_.feed(buffer, static_cast<size_t>(n));
    } else if (SSL_get_error(ssl_.get(), n) != SSL_ERROR_WANT_READ) {
      return std::nullopt;
    }
  }
}

void BenchClient::sign_up(const std::string& user, const std::string& password) {
  send({MsgType::Register, {user, password}});
  if (!expect(MsgType::Ok, std::chrono::seconds(60))) throw std::runtime_error("register timeout");
  login(user, password);
}

void BenchClient::login(const std::string& user, const std::string& password) {
  send({MsgType::Login, {user, password}});
  if (!expect(MsgType::Welcome, std::chrono::seconds(60))) throw std::runtime_error("login timeout");
  name_ = user;
}

void BenchClient::join(const std::string& room) {
  send({MsgType::Join, {room}});
  if (!expect(MsgType::SyncDone, std::chrono::seconds(30))) throw std::runtime_error("join timeout");
}

void BenchClient::make_nonblocking() { net::set_nonblocking(fd_.get()); }

void BenchClient::queue(const Message& message) { out_ += encode_frame(message); }

bool BenchClient::flush() {
  while (!out_.empty()) {
    ERR_clear_error();
    int n = SSL_write(ssl_.get(), out_.data(), static_cast<int>(std::min<size_t>(out_.size(), INT_MAX)));
    if (n > 0) {
      out_.erase(0, static_cast<size_t>(n));
      continue;
    }
    if (!would_block(SSL_get_error(ssl_.get(), n))) throw std::runtime_error("write failed");
    break;
  }
  return !out_.empty();
}

bool BenchClient::read_available(std::vector<Message>& out) {
  char buffer[16 * 1024];
  for (;;) {
    ERR_clear_error();
    int n = SSL_read(ssl_.get(), buffer, sizeof buffer);
    if (n <= 0) {
      if (would_block(SSL_get_error(ssl_.get(), n))) break;
      return false;
    }
    decoder_.feed(buffer, static_cast<size_t>(n));
  }
  while (auto message = decoder_.next()) out.push_back(std::move(*message));
  return true;
}

}  // namespace buzz::bench
