#pragma once

#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace buzz::net {

// Owns a file descriptor and closes it on destruction.
class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}
  ~UniqueFd() { reset(); }
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept;
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  int release();
  void reset(int fd = -1);

 private:
  int fd_ = -1;
};

struct Endpoint {
  std::string ip;
  uint16_t port = 0;

  bool operator==(const Endpoint& other) const { return ip == other.ip && port == other.port; }
  bool operator!=(const Endpoint& other) const { return !(*this == other); }
  std::string to_string() const { return ip + ":" + std::to_string(port); }
};

Endpoint endpoint_from(const sockaddr_storage& address);
// Fills an IPv4 socket address. Returns false when ip is not an IPv4 literal.
bool to_sockaddr(const Endpoint& endpoint, sockaddr_storage& address, socklen_t& length);

void set_nonblocking(int fd, bool enabled = true);
void set_no_delay(int fd);
void ignore_sigpipe();

// All sockets are IPv4. Port 0 picks a free port.
UniqueFd tcp_listen(uint16_t port, int backlog = 512);
UniqueFd udp_bind(uint16_t port);
uint16_t local_port(int fd);
Endpoint peer_endpoint(int fd);
// Blocking connect with a timeout. Throws std::system_error on failure.
UniqueFd tcp_connect(const std::string& host, uint16_t port, std::chrono::milliseconds timeout);

}  // namespace buzz::net
