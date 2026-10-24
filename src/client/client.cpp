#include "client.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>
#include <iostream>
#include <system_error>
#include <thread>

#include "buzz/crypto.hpp"
#include "buzz/p2p.hpp"
#include "buzz/validate.hpp"

namespace buzz::client {
namespace {

constexpr auto kConnectTimeout = std::chrono::seconds(5);
constexpr auto kMaxBackoff = std::chrono::seconds(30);
constexpr auto kHelloRetry = std::chrono::seconds(1);
// Keeps the NAT mapping toward the server alive.
constexpr auto kHelloKeepAlive = std::chrono::seconds(20);
constexpr int kPollMs = 100;

int64_t wall_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string clock_time(const std::string& timestamp_ms) {
  auto ms = parse_i64(timestamp_ms).value_or(0);
  std::time_t seconds = static_cast<std::time_t>(ms / 1000);
  std::tm local{};
  localtime_r(&seconds, &local);
  char out[8];
  std::strftime(out, sizeof out, "%H:%M", &local);
  return out;
}

std::string join_names(const std::vector<std::string>& names, size_t from = 0) {
  std::string out;
  for (size_t i = from; i < names.size(); ++i) out += (out.empty() ? "" : ", ") + names[i];
  return out.empty() ? "(none)" : out;
}

}  // namespace

Client::Client(const ClientConfig& config)
    : config_(config),
      link_(tls::make_client_context(config.ca_file), config.host, config.port),
      udp_(net::udp_bind(0)),
      peers_(udp_.get(), [this](const std::string& line) { print(line); }) {
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
    if (!link_.connected() && Clock::now() >= next_connect_) connect();
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

void Client::connect() {
  try {
    link_.connect(kConnectTimeout);
  } catch (const std::exception& e) {
    print("! cannot connect: " + std::string(e.what()) + " (retry in " +
          std::to_string(backoff_.count()) + "s)");
    next_connect_ = Clock::now() + backoff_;
    backoff_ = std::min(backoff_ * 2, std::chrono::seconds(kMaxBackoff));
    return;
  }
  backoff_ = std::chrono::seconds(1);
  print("* connected to " + link_.remote().to_string());
  if (!token_.empty()) {
    resuming_ = true;
    link_.send({MsgType::Resume, {token_}});
  }
}

void Client::on_disconnect() {
  udp_registered_ = false;
  server_udp_.reset();
  next_connect_ = Clock::now() + backoff_;
  print("! disconnected from server; direct messages to known peers still work");
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
        if (decode_body(data).type == MsgType::UdpAck) udp_registered_ = true;
      } catch (const ProtocolError&) {
      }
      continue;
    }
    peers_.on_datagram(data, from);
  }
}

void Client::send_udp_hello() {
  sockaddr_storage address{};
  socklen_t length = 0;
  if (!net::to_sockaddr(*server_udp_, address, length)) return;
  const std::string timestamp = std::to_string(wall_ms());
  std::string hello = encode_body(
      {MsgType::UdpHello, {user_, timestamp, p2p::hello_mac(token_, user_, timestamp)}});
  ::sendto(udp_.get(), hello.data(), hello.size(), 0, reinterpret_cast<sockaddr*>(&address),
           length);
}

void Client::tick(Clock::time_point now) {
  peers_.tick(now);
  if (token_.empty() || !link_.connected() || !server_udp_ || now < next_hello_) return;
  send_udp_hello();
  next_hello_ = now + (udp_registered_ ? kHelloKeepAlive : kHelloRetry);
}

void Client::on_message(const Message& m) {
  const auto& f = m.fields;
  switch (m.type) {
    case MsgType::Welcome: return on_welcome(m);
    case MsgType::Error: return on_error(m);
    case MsgType::Ok: return on_ok(m);
    case MsgType::RoomMsg: return on_room_message(m);
    case MsgType::PeerInfo: return on_peer_info(m);
    case MsgType::RoomEvent:
      return print("* " + m.field(2) + (m.field(1) == "join" ? " joined #" : " left #") +
                   m.field(0));
    case MsgType::SyncDone: {
      auto it = rooms_.find(m.field(0));
      if (it != rooms_.end()) it->second = std::max(it->second, parse_u64(m.field(1)).value_or(0));
      return;
    }
    case MsgType::Rooms: return print("rooms: " + join_names(f));
    case MsgType::Members: return print("#" + m.field(0) + ": " + join_names(f, 1));
    case MsgType::Pong: {
      auto sent = parse_i64(m.field(0)).value_or(wall_ms());
      return print("pong: " + std::to_string(wall_ms() - sent) + " ms");
    }
    default: return;
  }
}

void Client::on_welcome(const Message& m) {
  const std::string& user = m.field(0);
  if (user != user_) {
    rooms_.clear();
    current_room_.clear();
    peers_.clear();
  }
  user_ = user;
  token_ = m.field(1);
  peers_.set_self(user_);
  auto udp_port = parse_u64(m.field(2)).value_or(0);
  server_udp_ = net::Endpoint{link_.remote().ip, static_cast<uint16_t>(udp_port)};
  udp_registered_ = false;
  next_hello_ = Clock::now();
  print(resuming_ ? "* session resumed as " + user_ : "* logged in as " + user_);
  resuming_ = false;
  // Rejoin with the last seq seen, so the server replays only what we missed.
  for (const auto& [room, seq] : rooms_) {
    link_.send({MsgType::Join, {room, std::to_string(seq)}});
  }
}

void Client::on_error(const Message& m) {
  const std::string& context = m.field(0);
  if (context == "resume" || context == "session") {
    token_.clear();
    rooms_.clear();
    current_room_.clear();
    resuming_ = false;
  }
  if (context == "peer" && m.fields.size() > 2) waiting_dms_.erase(m.fields[2]);
  print("! " + context + ": " + m.field(1));
}

void Client::on_ok(const Message& m) {
  const std::string& context = m.field(0);
  const std::string& detail = m.field(1);
  if (context == "register") {
    print("* registered " + detail + "; now /login " + detail + " <password>");
  } else if (context == "join") {
    rooms_.emplace(detail, 0);
    if (current_room_.empty() || current_room_ == detail) {
      current_room_ = detail;
      print("* joined #" + detail);
    } else {
      print("* joined #" + detail + " (use /room " + detail + " to talk there)");
    }
  } else if (context == "leave") {
    rooms_.erase(detail);
    if (current_room_ == detail) current_room_ = rooms_.empty() ? "" : rooms_.begin()->first;
    print("* left #" + detail);
  } else if (context == "logout") {
    user_.clear();
    token_.clear();
    rooms_.clear();
    current_room_.clear();
    print("* logged out");
  }
}

void Client::on_room_message(const Message& m) {
  auto it = rooms_.find(m.field(0));
  auto seq = parse_u64(m.field(1));
  if (it == rooms_.end() || !seq || *seq <= it->second) return;  // Duplicate after a resync.
  it->second = *seq;
  print("[#" + it->first + " " + clock_time(m.field(2)) + "] " + m.field(3) + ": " + m.field(4));
}

void Client::on_peer_info(const Message& m) {
  const std::string& name = m.field(0);
  auto port = parse_u64(m.field(2));
  auto key = crypto::from_hex(m.field(3));
  if (!port || !key || key->size() != crypto::kKeySize) return;
  peers_.add_peer(name, {m.field(1), static_cast<uint16_t>(*port)}, std::move(*key));
  auto waiting = waiting_dms_.find(name);
  if (waiting == waiting_dms_.end()) return;
  for (const auto& text : waiting->second) peers_.send_text(name, text, Clock::now());
  waiting_dms_.erase(waiting);
}

void Client::print(const std::string& line) { std::cout << line << std::endl; }

}  // namespace buzz::client
