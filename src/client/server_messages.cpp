// Handling of messages from the server.
#include <ctime>

#include "buzz/p2p.hpp"
#include "buzz/validate.hpp"
#include "client.hpp"

namespace buzz::client {
namespace {

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

void Client::on_message(const Message& m) {
  switch (m.type) {
    case MsgType::Welcome: return on_welcome(m);
    case MsgType::Error: return on_error(m);
    case MsgType::Ok: return on_ok(m);
    case MsgType::RoomMsg: return on_room_message(m);
    case MsgType::PeerOffer: return on_peer_offer(m);
    case MsgType::PeerInfo: return on_peer_info(m);
    case MsgType::Relayed:
      if (valid_text(m.field(1))) print("[dm via server] " + m.field(0) + ": " + m.field(1));
      return;
    case MsgType::RoomEvent:
      if (m.field(1) == "join") contacts_.insert(m.field(2));
      return print("* " + m.field(2) + (m.field(1) == "join" ? " joined #" : " left #") +
                   m.field(0));
    case MsgType::SyncDone:
      return rooms_.on_synced(m.field(0), parse_u64(m.field(1)).value_or(0));
    case MsgType::Rooms: return print("rooms: " + join_names(m.fields));
    case MsgType::Members:
      for (size_t i = 1; i < m.fields.size(); ++i) contacts_.insert(m.fields[i]);
      return print("#" + m.field(0) + ": " + join_names(m.fields, 1));
    case MsgType::Pong: {
      auto sent = parse_i64(m.field(0)).value_or(wall_ms());
      return print("pong: " + std::to_string(wall_ms() - sent) + " ms");
    }
    default: return;
  }
}

void Client::on_welcome(const Message& m) {
  const std::string& user = m.field(0);
  if (user != user_) reset_identity();
  user_ = user;
  token_ = m.field(1);
  udp_key_ = crypto::sha256(token_);
  udp_nonce_ = m.field(3);
  hello_counter_ = 0;
  peers_.set_self(user_);
  auto udp_port = parse_u64(m.field(2)).value_or(0);
  server_udp_ = net::Endpoint{link_.remote().ip, static_cast<uint16_t>(udp_port)};
  udp_registered_ = false;
  next_hello_ = Clock::now();
  print(resuming_ ? "* session resumed as " + user_ : "* logged in as " + user_);
  resuming_ = false;
  // Rejoin from the last seq seen, so the server replays only what we missed.
  for (const auto& [room, seq] : rooms_.resume_points()) {
    link_.send({MsgType::Join, {room, std::to_string(seq)}});
  }
}

void Client::reset_identity() {
  rooms_.clear();
  peers_.clear();
  contacts_.clear();
}

void Client::on_error(const Message& m) {
  const std::string& context = m.field(0);
  if (context == "resume" || context == "session") {
    token_.clear();
    rooms_.clear();
    resuming_ = false;
  }
  if (context == "peer" && m.fields.size() > 2) return on_peer_error(m.fields[2], m.field(1));
  print("! " + context + ": " + m.field(1));
}

void Client::on_ok(const Message& m) {
  const std::string& context = m.field(0);
  const std::string& detail = m.field(1);
  if (context == "register") {
    print("* registered " + detail + "; now /login " + detail + " <password>");
  } else if (context == "join") {
    bool current = rooms_.on_joined(detail);
    print("* joined #" + detail + (current ? "" : " (use /room " + detail + " to talk there)"));
  } else if (context == "leave") {
    rooms_.on_left(detail);
    print("* left #" + detail);
  } else if (context == "logout") {
    user_.clear();
    token_.clear();
    reset_identity();
    print("* logged out");
  }
}

void Client::on_room_message(const Message& m) {
  auto seq = parse_u64(m.field(1));
  if (!seq || !rooms_.accept(m.field(0), *seq)) return;  // Duplicate after a resync.
  contacts_.insert(m.field(3));
  print("[#" + m.field(0) + " " + clock_time(m.field(2)) + "] " + m.field(3) + ": " + m.field(4));
}

// Someone asked for a direct channel. Only contacts learn our address;
// others are declined and their messages arrive through the relay.
void Client::on_peer_offer(const Message& m) {
  const std::string& from = m.field(0);
  if (contacts_.count(from) == 0 ||
      !connect_peer(from, m.field(1), m.field(2), m.field(3))) {
    link_.send({MsgType::PeerDecline, {from}});
    print("* " + from + " asked for a direct connection; declined because you share no room. "
          "/dm " + from + " to allow it.");
    return;
  }
  link_.send({MsgType::PeerAccept, {from, crypto::to_hex(keys_.public_key())}});
  for (const auto& text : dms_.take(from)) peers_.send_text(from, text, Clock::now());
}

void Client::on_peer_info(const Message& m) {
  const std::string& name = m.field(0);
  if (!connect_peer(name, m.field(1), m.field(2), m.field(3))) return;
  for (const auto& text : dms_.take(name)) peers_.send_text(name, text, Clock::now());
}

bool Client::connect_peer(const std::string& name, const std::string& ip, const std::string& port,
                          const std::string& public_hex) {
  auto port_number = parse_u64(port);
  auto public_key = crypto::from_hex(public_hex);
  if (!port_number || *port_number > 65535 || !public_key) return false;
  auto key = p2p::derive_session_key(keys_, user_, name, *public_key);
  if (!key) return false;
  peers_.add_peer(name, {ip, static_cast<uint16_t>(*port_number)}, std::move(*key),
                  std::move(*public_key));
  return true;
}

// The server could not introduce us. If the peer is online but has no
// usable UDP path, or declined, the queued messages go through the relay.
void Client::on_peer_error(const std::string& peer, const std::string& reason) {
  std::vector<std::string> texts = dms_.take(peer);
  if (texts.empty() || reason.find("not online") != std::string::npos) {
    print("! dm: " + reason + (texts.empty() ? "" : "; message not delivered"));
    return;
  }
  for (const auto& text : texts) relay(peer, text);
}

}  // namespace buzz::client
