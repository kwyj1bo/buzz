#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iosfwd>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "buzz/net.hpp"
#include "peers.hpp"
#include "server_link.hpp"

namespace buzz::client {

struct ClientConfig {
  std::string host = "127.0.0.1";
  uint16_t port = 7000;
  std::string ca_file;  // Empty means --insecure.
};

// Interactive client. The input thread only queues lines; one network thread
// owns all state and polls the TLS link, the UDP socket and a wake pipe.
class Client {
 public:
  explicit Client(const ClientConfig& config);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Reads commands until /quit or end of input.
  void run(std::istream& input);

 private:
  using Clock = std::chrono::steady_clock;

  // Network thread (client.cpp).
  void network_loop();
  void poll_once();
  void connect();
  void on_disconnect();
  void read_udp();
  void send_udp_hello();
  void tick(Clock::time_point now);
  void on_message(const Message& m);
  void on_welcome(const Message& m);
  void on_error(const Message& m);
  void on_ok(const Message& m);
  void on_room_message(const Message& m);
  void on_peer_info(const Message& m);
  void print(const std::string& line);

  // Commands (commands.cpp).
  void submit(std::string line);
  void process_lines();
  void handle_line(const std::string& line);
  void cmd_join(const std::vector<std::string>& args);
  void cmd_leave(const std::vector<std::string>& args);
  void cmd_room(const std::vector<std::string>& args);
  void cmd_dm(const std::string& peer, const std::string& text);
  void say(const std::string& text);
  bool require_link();
  bool require_login();

  ClientConfig config_;
  ServerLink link_;
  net::UniqueFd udp_;
  net::UniqueFd wake_read_;
  net::UniqueFd wake_write_;
  PeerManager peers_;

  std::string user_;
  std::string token_;
  bool resuming_ = false;
  std::optional<net::Endpoint> server_udp_;
  bool udp_registered_ = false;
  Clock::time_point next_hello_{};
  Clock::time_point next_connect_{};
  std::chrono::seconds backoff_{1};

  std::map<std::string, uint64_t> rooms_;  // Joined room to last seq seen.
  std::string current_room_;
  std::map<std::string, std::vector<std::string>> waiting_dms_;

  std::mutex lines_mutex_;
  std::deque<std::string> lines_;
  std::atomic<bool> running_{true};
};

}  // namespace buzz::client
