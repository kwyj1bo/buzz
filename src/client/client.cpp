#include "client.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <system_error>
#include <thread>

#include "buzz/p2p.hpp"

namespace buzz::client {
namespace {

constexpr auto kConnectTimeout = std::chrono::seconds(5);
constexpr auto kHelloRetry = std::chrono::seconds(1);
// Keeps the NAT mapping toward the server alive.
constexpr auto kHelloKeepAlive = std::chrono::seconds(20);
constexpr int kPollMs = 100;

}  // namespace

Client::Client(const ClientConfig& config)
    : config_(config),
      link_(tls::make_client_context(config.ca_file), config.host, config.port),
      udp_(net::udp_bind(0)),
      peers_(
          udp_.get(), [this](const std::string& line) { print(line); },
          [this](const std::string& peer, const std::string& text) { relay(peer, text); }) {
  int fds[2];
  if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
    throw std::system_error(errno, std::generic_category(), "pipe2");
  }
  wake_read_.reset(fds[0]);
  wake_write_.reset(fds[1]);
}

Client::~Client() = default;

void Client::run(std::istream& input) {
  net::ignore_sigpipe();
  print("buzz: connecting to " + config_.host + ":" + std::to_string(config_.port) +
        " (type /help)");
  std::thread network([this] { network_loop(); });
  std::string line;
  while (running_ && std::getline(input, line)) submit(line);
  submit("/quit");
  network.join();
}

void Client::network_loop() {
  while (running_) {
    if (!link_.connected() && connect_policy_.should_connect(!token_.empty(), Clock::now())) {
      connect();
    }
    poll_once();
    tick(Clock::now());
  }
  link_.flush();
  link_.close();
}

void Client::poll_once() {
  pollfd fds[3] = {{wake_read_.get(), POLLIN, 0}, {udp_.get(), POLLIN, 0}, {-1, 0, 0}};
  nfds_t count = 2;
  if (link_.connected()) {
    fds[2] = {link_.fd(), static_cast<short>(POLLIN | (link_.wants_write() ? POLLOUT : 0)), 0};
    count = 3;
  }
  if (::poll(fds, count, kPollMs) <= 0) return;
  if (fds[0].revents) process_lines();
  if (fds[1].revents & POLLIN) read_udp();
  if (count == 3 && fds[2].revents && link_.connected()) {
    for (const auto& message : link_.receive()) on_message(message);
    link_.flush();
    if (!link_.connected()) on_disconnect();
  }
}

bool Client::connect() {
  try {
    link_.connect(kConnectTimeout);
  } catch (const std::exception& e) {
    print("! cannot connect: " + std::string(e.what()) + " (retry in " +
          std::to_string(connect_policy_.backoff().count()) + "s)");
    connect_policy_.on_failed(Clock::now());
    return false;
  }
  connect_policy_.on_connected();
  print("* connected to " + link_.remote().to_string());
  if (!token_.empty()) {
    resuming_ = true;
    link_.send({MsgType::Resume, {token_}});
  }
  return true;
}

void Client::on_disconnect() {
  udp_registered_ = false;
  server_udp_.reset();
  // Introductions in flight will never be answered; ask again on reconnect.
  dms_.on_disconnect();
  connect_policy_.on_disconnected(Clock::now());
  if (token_.empty()) {
    print("* disconnected from server; it reconnects when you run a command");
  } else {
    print("! disconnected from server, reconnecting; direct messages to known peers still work");
  }
}

void Client::read_udp() {
  char buffer[2048];
  for (;;) {
    sockaddr_storage address{};
    socklen_t length = sizeof address;
    ssize_t n = ::recvfrom(udp_.get(), buffer, sizeof buffer, MSG_DONTWAIT,
                           reinterpret_cast<sockaddr*>(&address), &length);
    if (n <= 0) return;
    std::string_view data(buffer, static_cast<size_t>(n));
    net::Endpoint from = net::endpoint_from(address);
    if (server_udp_ && from == *server_udp_) {
      try {
        if (decode_body(data).type == MsgType::UdpAck && !udp_registered_) on_udp_registered();
      } catch (const ProtocolError&) {
      }
      continue;
    }
    peers_.on_datagram(data, from);
  }
}

// Introductions need our UDP endpoint, so queued requests go out now.
void Client::on_udp_registered() {
  udp_registered_ = true;
  for (const auto& peer : dms_.request_again(Clock::now())) request_peer(peer);
}

void Client::send_udp_hello() {
  sockaddr_storage address{};
  socklen_t length = 0;
  if (!net::to_sockaddr(*server_udp_, address, length)) return;
  const std::string counter = std::to_string(++hello_counter_);
  std::string hello = encode_body(
      {MsgType::UdpHello, {user_, counter, p2p::hello_mac(udp_key_, user_, udp_nonce_, counter)}});
  ::sendto(udp_.get(), hello.data(), hello.size(), 0, reinterpret_cast<sockaddr*>(&address),
           length);
}

void Client::tick(Clock::time_point now) {
  peers_.tick(now);
  for (auto& [peer, texts] : dms_.take_expired(now)) {
    print("* no direct path to " + peer + " in time");
    for (const auto& text : texts) relay(peer, text);
  }
  if (token_.empty() || !link_.connected() || !server_udp_ || now < next_hello_) return;
  send_udp_hello();
  next_hello_ = now + (udp_registered_ ? kHelloKeepAlive : kHelloRetry);
}

void Client::request_peer(const std::string& peer) {
  link_.send({MsgType::PeerRequest, {peer, crypto::to_hex(keys_.public_key())}});
}

void Client::relay(const std::string& peer, const std::string& text) {
  if (!link_.connected() || token_.empty()) {
    print("! message to " + peer + " was not delivered: " + text);
    return;
  }
  print("* relaying to " + peer + " through the server");
  link_.send({MsgType::Relay, {peer, text}});
}

void Client::print(const std::string& line) { std::cout << line << std::endl; }

}  // namespace buzz::client
