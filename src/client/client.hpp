#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "buzz/crypto.hpp"
#include "buzz/net.hpp"
#include "connect_policy.hpp"
#include "dm_queue.hpp"
#include "peers.hpp"
#include "room_state.hpp"
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
  bool connect();
  void on_disconnect();
  void read_udp();
  void on_udp_registered();
  void send_udp_hello();
  void tick(Clock::time_point now);
  void request_peer(const std::string& peer);
  void relay(const std::string& peer, const std::string& text);
  void print(const std::string& line);

  // Server messages (server_messages.cpp).
  void on_message(const Message& m);
  void on_welcome(const Message& m);
  void on_error(const Message& m);
  void on_ok(const Message& m);
  void on_room_message(const Message& m);
  void on_peer_offer(const Message& m);
  void on_peer_info(const Message& m);
  void on_peer_error(const std::string& peer, const std::string& reason);
  bool connect_peer(const std::string& name, const std::string& ip, const std::string& port,
                    const std::string& public_hex);
  void reset_identity();

  // Commands (commands.cpp).
  void submit(std::string line);
  void process_lines();
  void handle_line(const std::string& line);
  void cmd_join(const std::vector<std::string>& args);
  void cmd_leave(const std::vector<std::string>& args);
  void cmd_room(const std::vector<std::string>& args);
  void cmd_dm(const std::string& peer, const std::string& text);
  void cmd_verify(const std::vector<std::string>& args);
  void say(const std::string& text);
  bool require_link();
  bool require_login();

  ClientConfig config_;
  ServerLink link_;
  net::UniqueFd udp_;
  net::UniqueFd wake_read_;
  net::UniqueFd wake_write_;
  // Fresh for each run of the client; peers derive session keys from it.
  crypto::X25519KeyPair keys_;
  PeerManager peers_;
  RoomState rooms_;
  DmQueue dms_;
  ConnectPolicy connect_policy_;
  // Users we share a room with or messaged; only they get our UDP address.
  std::set<std::string> contacts_;

  std::string user_;
  std::string token_;
  std::string udp_key_;    // SHA-256 of the token; signs UDP hellos.
  std::string udp_nonce_;  // From Welcome.
  uint64_t hello_counter_ = 0;
  bool resuming_ = false;
  std::optional<net::Endpoint> server_udp_;
  bool udp_registered_ = false;
  Clock::time_point next_hello_{};

  std::mutex lines_mutex_;
  std::deque<std::string> lines_;
  std::atomic<bool> running_{true};
};

}  // namespace buzz::client
