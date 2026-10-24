#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "buzz/net.hpp"
#include "buzz/protocol.hpp"
#include "buzz/reliable_channel.hpp"

namespace buzz::client {

// Direct messages over UDP. Each peer has a key from the server, and every
// datagram is encrypted and acknowledged, so chats keep working even while
// the server is down. Messages that are never acknowledged go to the
// `undelivered` callback, which relays them through the server.
class PeerManager {
 public:
  using Clock = std::chrono::steady_clock;
  using Print = std::function<void(const std::string&)>;
  using Undelivered = std::function<void(const std::string& peer, const std::string& text)>;

  PeerManager(int udp_fd, Print print, Undelivered undelivered);

  void set_self(const std::string& user) { self_ = user; }
  void add_peer(const std::string& name, const net::Endpoint& endpoint, std::string key);
  bool has_peer(const std::string& name) const { return peers_.count(name) != 0; }
  void send_text(const std::string& name, const std::string& text, Clock::time_point now);
  void on_datagram(std::string_view data, const net::Endpoint& from);
  // Sends due packets and reports messages that were never acknowledged.
  void tick(Clock::time_point now);
  std::vector<std::string> describe() const;
  void clear() { peers_.clear(); }

 private:
  struct Peer {
    net::Endpoint endpoint;
    std::string key;
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
