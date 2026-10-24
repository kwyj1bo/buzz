#include "buzz/net.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <system_error>

namespace buzz::net {
namespace {

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::system_error(errno, std::generic_category(), what);
}

sockaddr_in any_address(uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  return address;
}

UniqueFd bound_socket(int type, uint16_t port) {
  UniqueFd fd(::socket(AF_INET, type | SOCK_CLOEXEC, 0));
  if (!fd.valid()) throw_errno("socket");
  int on = 1;
  ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  sockaddr_in address = any_address(port);
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
    throw_errno("bind port " + std::to_string(port));
  }
  return fd;
}

sockaddr_in resolve(const std::string& host, uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
  if (rc != 0) {
    throw std::system_error(EHOSTUNREACH, std::generic_category(),
                            "resolve " + host + ": " + ::gai_strerror(rc));
  }
  sockaddr_in address = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
  ::freeaddrinfo(result);
  address.sin_port = htons(port);
  return address;
}

void wait_for_connect(int fd, std::chrono::milliseconds timeout) {
  pollfd pfd{fd, POLLOUT, 0};
  int rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
  if (rc == 0) throw std::system_error(ETIMEDOUT, std::generic_category(), "connect");
  if (rc < 0) throw_errno("poll");
  int error = 0;
  socklen_t length = sizeof error;
  ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
  if (error != 0) throw std::system_error(error, std::generic_category(), "connect");
}

}  // namespace

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
  if (this != &other) reset(other.release());
  return *this;
}

int UniqueFd::release() {
  int fd = fd_;
  fd_ = -1;
  return fd;
}

void UniqueFd::reset(int fd) {
  if (fd_ >= 0) ::close(fd_);
  fd_ = fd;
}

UniqueFd tcp_listen(uint16_t port, int backlog) {
  UniqueFd fd = bound_socket(SOCK_STREAM, port);
  if (::listen(fd.get(), backlog) != 0) throw_errno("listen");
  set_nonblocking(fd.get());
  return fd;
}

UniqueFd udp_bind(uint16_t port) {
  UniqueFd fd = bound_socket(SOCK_DGRAM, port);
  set_nonblocking(fd.get());
  return fd;
}

UniqueFd tcp_connect(const std::string& host, uint16_t port, std::chrono::milliseconds timeout) {
  sockaddr_in address = resolve(host, port);
  UniqueFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd.valid()) throw_errno("socket");
  set_nonblocking(fd.get());
  if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
    if (errno != EINPROGRESS) throw_errno("connect");
    wait_for_connect(fd.get(), timeout);
  }
  set_nonblocking(fd.get(), false);
  set_no_delay(fd.get());
  return fd;
}

uint16_t local_port(int fd) {
  sockaddr_in address{};
  socklen_t length = sizeof address;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    throw_errno("getsockname");
  }
  return ntohs(address.sin_port);
}

Endpoint peer_endpoint(int fd) {
  sockaddr_storage address{};
  socklen_t length = sizeof address;
  if (::getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) return {};
  return endpoint_from(address);
}

Endpoint endpoint_from(const sockaddr_storage& address) {
  if (address.ss_family != AF_INET) return {};
  const auto& in = reinterpret_cast<const sockaddr_in&>(address);
  char ip[INET_ADDRSTRLEN] = {};
  ::inet_ntop(AF_INET, &in.sin_addr, ip, sizeof ip);
  return {ip, ntohs(in.sin_port)};
}

bool to_sockaddr(const Endpoint& endpoint, sockaddr_storage& address, socklen_t& length) {
  std::memset(&address, 0, sizeof address);
  auto& in = reinterpret_cast<sockaddr_in&>(address);
  in.sin_family = AF_INET;
  in.sin_port = htons(endpoint.port);
  if (::inet_pton(AF_INET, endpoint.ip.c_str(), &in.sin_addr) != 1) return false;
  length = sizeof in;
  return true;
}

void set_nonblocking(int fd, bool enabled) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) throw_errno("fcntl");
  flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (::fcntl(fd, F_SETFL, flags) != 0) throw_errno("fcntl");
}

void set_no_delay(int fd) {
  int on = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

// A write to a closed TLS socket must return EPIPE, not kill the process.
void ignore_sigpipe() { std::signal(SIGPIPE, SIG_IGN); }

}  // namespace buzz::net
