#include "support/test_client.hpp"

#include <openssl/err.h>
#include <poll.h>

#include <stdexcept>

namespace buzz::test {

TestClient::TestClient(const std::string& ca_file, uint16_t port)
    : ctx_(tls::make_client_context(ca_file)) {
  fd_ = net::tcp_connect("127.0.0.1", port, 5s);
  ssl_ = tls::make_client_ssl(ctx_.get(), fd_.get(), "127.0.0.1");
  if (SSL_connect(ssl_.get()) != 1) throw std::runtime_error("handshake: " + tls::last_error());
}

void TestClient::send(const Message& message) { send_raw(encode_frame(message)); }

void TestClient::send_raw(const std::string& bytes) {
  if (SSL_write(ssl_.get(), bytes.data(), static_cast<int>(bytes.size())) <= 0) {
    throw std::runtime_error("write failed");
  }
}

std::optional<Message> TestClient::receive(std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (auto message = decoder_.next()) return message;
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left <= 0ms || read_some(left) != ReadResult::Data) return std::nullopt;
  }
}

std::optional<Message> TestClient::expect(MsgType type, std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left <= 0ms) return std::nullopt;
    auto message = receive(left);
    if (!message || message->type == type) return message;
  }
}

bool TestClient::closed_by_server(std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left <= 0ms) return false;
    switch (read_some(left)) {
      case ReadResult::Closed: return true;
      case ReadResult::Timeout: return false;
      case ReadResult::Data: break;
    }
  }
}

std::string TestClient::sign_up(const std::string& user, const std::string& password) {
  send({MsgType::Register, {user, password}});
  auto registered = receive();
  if (!registered || registered->type != MsgType::Ok) throw std::runtime_error("register failed");
  send({MsgType::Login, {user, password}});
  auto welcome = expect(MsgType::Welcome);
  if (!welcome) throw std::runtime_error("login failed");
  welcome_ = *welcome;
  return welcome->field(1);
}

TestClient::ReadResult TestClient::read_some(std::chrono::milliseconds timeout) {
  if (SSL_pending(ssl_.get()) == 0) {
    pollfd pfd{fd_.get(), POLLIN, 0};
    if (::poll(&pfd, 1, static_cast<int>(timeout.count())) <= 0) return ReadResult::Timeout;
  }
  char buffer[16 * 1024];
  ERR_clear_error();
  int n = SSL_read(ssl_.get(), buffer, sizeof buffer);
  if (n <= 0) {
    // WANT_READ here means a non-data record (such as a session ticket) was
    // consumed; the caller polls again.
    int error = SSL_get_error(ssl_.get(), n);
    return error == SSL_ERROR_WANT_READ ? ReadResult::Data : ReadResult::Closed;
  }
  decoder_.feed(buffer, static_cast<size_t>(n));
  return ReadResult::Data;
}

}  // namespace buzz::test
