#include "server_link.hpp"

#include <openssl/err.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <climits>
#include <stdexcept>

namespace buzz::client {
namespace {

constexpr size_t kReadChunk = 16 * 1024;

void set_timeouts(int fd, std::chrono::milliseconds timeout) {
  timeval tv{};
  tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
  tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

bool would_block(int ssl_error) {
  return ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE;
}

}  // namespace

ServerLink::ServerLink(tls::ContextPtr ctx, std::string host, uint16_t port)
    : ctx_(std::move(ctx)), host_(std::move(host)), port_(port) {}

void ServerLink::connect(std::chrono::milliseconds timeout) {
  close();
  fd_ = net::tcp_connect(host_, port_, timeout);
  remote_ = net::peer_endpoint(fd_.get());
  set_timeouts(fd_.get(), timeout);
  ssl_ = tls::make_client_ssl(ctx_.get(), fd_.get(), host_);
  try {
    handshake();
  } catch (...) {
    close();
    throw;
  }
  set_timeouts(fd_.get(), std::chrono::milliseconds(0));
  net::set_nonblocking(fd_.get());
}

void ServerLink::handshake() {
  ERR_clear_error();
  if (SSL_connect(ssl_.get()) == 1) return;
  long verify = SSL_get_verify_result(ssl_.get());
  std::string reason = verify != X509_V_OK ? X509_verify_cert_error_string(verify)
                                           : tls::last_error();
  throw std::runtime_error("TLS handshake failed: " + reason);
}

void ServerLink::close() {
  if (ssl_) SSL_shutdown(ssl_.get());
  ssl_.reset();
  fd_.reset();
  decoder_ = FrameDecoder();
  out_.clear();
}

void ServerLink::send(const Message& message) {
  if (!connected()) return;
  out_ += encode_frame(message);
  flush();
}

std::vector<Message> ServerLink::receive() {
  std::vector<Message> messages;
  char buffer[kReadChunk];
  while (connected()) {
    ERR_clear_error();
    int n = SSL_read(ssl_.get(), buffer, sizeof buffer);
    if (n <= 0) {
      if (!would_block(SSL_get_error(ssl_.get(), n))) close();
      break;
    }
    decoder_.feed(buffer, static_cast<size_t>(n));
    try {
      while (auto message = decoder_.next()) messages.push_back(std::move(*message));
    } catch (const ProtocolError&) {
      close();
    }
  }
  return messages;
}

void ServerLink::flush() {
  while (connected() && !out_.empty()) {
    ERR_clear_error();
    int chunk = static_cast<int>(std::min<size_t>(out_.size(), INT_MAX));
    int n = SSL_write(ssl_.get(), out_.data(), chunk);
    if (n > 0) {
      out_.erase(0, static_cast<size_t>(n));
      continue;
    }
    if (!would_block(SSL_get_error(ssl_.get(), n))) close();
    return;
  }
}

}  // namespace buzz::client
