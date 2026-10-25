#include "peers.hpp"

#include <sys/socket.h>

#include <utility>

#include "buzz/p2p.hpp"
#include "buzz/validate.hpp"

namespace buzz::client {

PeerManager::PeerManager(int udp_fd, Print print, Undelivered undelivered)
    : udp_fd_(udp_fd), print_(std::move(print)), undelivered_(std::move(undelivered)) {}

void PeerManager::add_peer(const std::string& name, const net::Endpoint& endpoint,
                           std::string key, std::string public_key) {
  Peer& peer = peers_[name];
  peer.endpoint = endpoint;
  peer.key = std::move(key);
  peer.public_key = std::move(public_key);
  // A new introduction may mean the peer restarted and reset its numbering.
  peer.window = ReceiveWindow();
  // The first packet opens our NAT mapping toward the peer.
  send(peer, {MsgType::DmPunch, {self_}});
  print_("* end-to-end encrypted channel to " + name + " ready (" + endpoint.to_string() +
         "); /verify " + name + " to compare safety numbers");
}

std::optional<std::string> PeerManager::public_key(const std::string& name) const {
  auto it = peers_.find(name);
  if (it == peers_.end()) return std::nullopt;
  return it->second.public_key;
}

void PeerManager::send_text(const std::string& name, const std::string& text,
                            Clock::time_point now) {
  auto it = peers_.find(name);
  if (it == peers_.end()) return;
  it->second.sender.enqueue(text, now);
  tick(now);
}

void PeerManager::on_datagram(std::string_view data, const net::Endpoint& from) {
  auto sender = p2p::peek_sender(data);
  if (!sender) return;
  auto it = peers_.find(*sender);
  if (it == peers_.end()) return;
  auto message = p2p::open_datagram(data, it->second.key);
  if (!message) return;
  try {
    if (message->field(0) != *sender) return;
    // Authenticated, so follow the peer if its address changed.
    it->second.endpoint = from;
    handle(*sender, it->second, *message);
  } catch (const ProtocolError&) {
  }
}

void PeerManager::handle(const std::string& name, Peer& peer, const Message& message) {
  if (message.type == MsgType::DmAck) {
    if (auto seq = parse_u64(message.field(1))) peer.sender.ack(*seq);
    return;
  }
  if (message.type != MsgType::DmData) return;
  auto seq = parse_u64(message.field(1));
  if (!seq) return;
  // Ack every copy: the first ack may have been lost.
  send(peer, {MsgType::DmAck, {self_, message.field(1)}});
  const std::string& text = message.field(2);
  if (peer.window.accept(*seq) && valid_text(text)) print_("[dm] " + name + ": " + text);
}

void PeerManager::tick(Clock::time_point now) {
  std::map<std::string, std::vector<std::string>> lost;
  for (auto& [name, peer] : peers_) {
    for (const auto& packet : peer.sender.due(now)) {
      send(peer, {MsgType::DmData, {self_, std::to_string(packet.seq), packet.payload}});
    }
    auto failed = peer.sender.take_failed();
    if (failed.empty()) continue;
    // The peer is unreachable: hand over its whole queue, oldest first.
    auto& texts = lost[name];
    for (auto& packet : failed) texts.push_back(std::move(packet.payload));
    for (auto& packet : peer.sender.take_all()) texts.push_back(std::move(packet.payload));
  }
  // Callbacks run after the loop because they may change peers_.
  for (const auto& [name, texts] : lost) {
    peers_.erase(name);
    for (const auto& text : texts) undelivered_(name, text);
  }
}

std::vector<std::string> PeerManager::describe() const {
  std::vector<std::string> out;
  for (const auto& [name, peer] : peers_) {
    out.push_back(name + " at " + peer.endpoint.to_string() + ", " +
                  std::to_string(peer.sender.in_flight()) + " unacked");
  }
  return out;
}

void PeerManager::send(const Peer& peer, const Message& message) {
  sockaddr_storage address{};
  socklen_t length = 0;
  if (self_.empty() || !net::to_sockaddr(peer.endpoint, address, length)) return;
  std::string datagram = p2p::seal_datagram(self_, peer.key, message);
  ::sendto(udp_fd_, datagram.data(), datagram.size(), 0, reinterpret_cast<sockaddr*>(&address),
           length);
}

}  // namespace buzz::client
