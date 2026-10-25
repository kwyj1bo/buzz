#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "buzz/net.hpp"
#include "buzz/protocol.hpp"
#include "buzz/reliable_channel.hpp"

namespace buzz::client {

// Direct messages over UDP. Each peer has a session key that the two
// clients derived with X25519; every datagram is encrypted and acknowledged,
// so chats keep working even while the server is down.
// A peer whose messages are never acknowledged is dropped (it may have
// restarted with a new address and key) and the messages go to the
// `undelivered` callback, which relays them through the server.
class PeerManager {
 public:
  using Clock = std::chrono::steady_clock;
  using Print = std::function<void(const std::string&)>;
  using Undelivered = std::function<void(const std::string& peer, const std::string& text)>;

  PeerManager(int udp_fd, Print print, Undelivered undelivered);

  void set_self(const std::string& user) { self_ = user; }
  void add_peer(const std::string& name, const net::Endpoint& endpoint, std::string key,
                std::string public_key);
  void remove(const std::string& name) { peers_.erase(name); }
  bool has_peer(const std::string& name) const { return peers_.count(name) != 0; }
  // The peer's raw X25519 public key, for safety numbers.
  std::optional<std::string> public_key(const std::string& name) const;
  void send_text(const std::string& name, const std::string& text, Clock::time_point now);
  void on_datagram(std::string_view data, const net::Endpoint& from);
  // Sends due packets, then drops peers with lost messages and reports them.
  void tick(Clock::time_point now);
  std::vector<std::string> describe() const;
  void clear() { peers_.clear(); }

 private:
  struct Peer {
    net::Endpoint endpoint;
    std::string key;
    std::string public_key;
    // About 1.75 s of retries before falling back to the relay.
    ReliableSender sender{std::chrono::milliseconds(250), 4};
    ReceiveWindow window;
  };

  void handle(const std::string& name, Peer& peer, const Message& message);
  void send(const Peer& peer, const Message& message);

  int udp_fd_;
  Print print_;
  Undelivered undelivered_;
  std::string self_;
  std::map<std::string, Peer> peers_;
};

}  // namespace buzz::client
