#include <sys/socket.h>

#include <cerrno>
#include <chrono>

#include "buzz/crypto.hpp"
#include "buzz/log.hpp"
#include "buzz/p2p.hpp"
#include "buzz/server.hpp"
#include "buzz/validate.hpp"
#include "connection.hpp"

namespace buzz {
namespace {

constexpr size_t kJoinHistory = 50;

Message error(MsgType context, const std::string& reason) {
  return {MsgType::Error, {to_string(context), reason}};
}

Message ok(MsgType context, const std::string& detail = "") {
  return {MsgType::Ok, {to_string(context), detail}};
}

Message room_message(const std::string& room, const StoredMessage& m) {
  return {MsgType::RoomMsg,
          {room, std::to_string(m.seq), std::to_string(m.timestamp_ms), m.from, m.text}};
}

int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

void Server::handle(Connection& c, const Message& m) {
  switch (m.type) {
    case MsgType::Register: return on_register(c, m);
    case MsgType::Login: return on_login(c, m);
    case MsgType::Resume: return on_resume(c, m);
    case MsgType::Ping: return send(c, {MsgType::Pong, m.fields});
    default: break;
  }
  if (c.user.empty()) return send(c, error(m.type, "not logged in"));
  switch (m.type) {
    case MsgType::Logout: return on_logout(c);
    case MsgType::Join: return on_join(c, m);
    case MsgType::Leave: return on_leave(c, m);
    case MsgType::Say: return on_say(c, m);
    case MsgType::Who: return on_who(c, m);
    case MsgType::ListRooms: return on_list_rooms(c);
    case MsgType::PeerRequest: return on_peer_request(c, m);
    default: return send(c, error(m.type, "unexpected message"));
  }
}

void Server::on_register(Connection& c, const Message& m) {
  if (c.auth_pending) return send(c, error(m.type, "authentication already in progress"));
  std::string user = m.field(0);
  std::string password = m.field(1);
  if (!valid_username(user)) {
    return send(c, error(m.type, "username must be 3-32 letters, digits, '_' or '-'"));
  }
  if (!valid_password(password)) {
    return send(c, error(m.type, "password must be 8-128 characters"));
  }
  c.auth_pending = true;
  pool_.submit([this, id = c.id, user = std::move(user), password = std::move(password)]() mutable {
    auto result = users_.add_user(user, password);
    crypto::wipe(password);
    bool created = result == UserStore::RegisterResult::Ok;
    post_result({id, MsgType::Register, user, created, created ? "" : "username is taken"});
  });
}

void Server::on_login(Connection& c, const Message& m) {
  if (c.auth_pending) return send(c, error(m.type, "authentication already in progress"));
  c.auth_pending = true;
  pool_.submit([this, id = c.id, user = m.field(0), password = m.field(1)]() mutable {
    bool ok = users_.authenticate(user, password);
    crypto::wipe(password);
    post_result({id, MsgType::Login, user, ok, ok ? "" : "invalid username or password"});
  });
}

void Server::process_auth_results() {
  std::vector<AuthResult> batch;
  {
    std::lock_guard lock(results_mutex_);
    batch.swap(results_);
  }
  for (AuthResult& r : batch) {
    Connection* c = find_connection(r.conn_id);
    if (!c || c->closing) continue;
    c->auth_pending = false;
    if (!r.ok) {
      send(*c, error(r.kind, r.error));
    } else if (r.kind == MsgType::Register) {
      BUZZ_LOG(Info, "registered " << r.user);
      send(*c, ok(MsgType::Register, r.user));
    } else {
      start_session(*c, r.user);
    }
  }
}

void Server::start_session(Connection& c, const std::string& user) {
  if (!c.user.empty()) end_session(c);
  if (auto old = sessions_.remove_user(user)) {
    take_over_session(*old, c.id, "logged in from another connection");
  }
  Session& session = sessions_.create(user, c.id);
  c.user = user;
  BUZZ_LOG(Info, user << " logged in from " << c.peer);
  send(c, {MsgType::Welcome, {user, session.token, std::to_string(udp_port_)}});
}

void Server::on_resume(Connection& c, const Message& m) {
  Session* session = sessions_.find_token(m.field(0));
  if (!session) return send(c, error(m.type, "session expired, log in again"));
  const std::string user = session->user;
  const std::string token = session->token;
  if (c.user != user) {
    if (!c.user.empty()) end_session(c);
    take_over_session(*sessions_.find_user(user), c.id, "session resumed elsewhere");
    sessions_.attach(user, c.id);
    c.user = user;
  }
  BUZZ_LOG(Info, user << " resumed from " << c.peer);
  send(c, {MsgType::Welcome, {user, token, std::to_string(udp_port_)}});
}

// Moves a user from an old connection to a new one. The old connection is
// told why and closed; the user leaves its rooms so membership starts clean.
void Server::take_over_session(const Session& old, uint64_t new_conn_id, const char* reason) {
  if (old.conn_id == 0 || old.conn_id == new_conn_id) return;
  leave_all_rooms(old.user);
  if (Connection* previous = find_connection(old.conn_id)) {
    send(*previous, {MsgType::Error, {"session", reason}});
    previous->user.clear();
    mark_close(*previous);
  }
}

void Server::on_logout(Connection& c) {
  BUZZ_LOG(Info, c.user << " logged out");
  end_session(c);
  send(c, ok(MsgType::Logout));
}

void Server::end_session(Connection& c) {
  leave_all_rooms(c.user);
  sessions_.remove_user(c.user);
  c.user.clear();
}

// The session stays for session_ttl so the client can resume it.
void Server::detach_session(Connection& c) {
  if (c.user.empty()) return;
  Session* session = sessions_.find_user(c.user);
  if (session && session->conn_id == c.id) {
    sessions_.detach(c.user, SessionManager::Clock::now());
    leave_all_rooms(c.user);
  }
  c.user.clear();
}

void Server::on_join(Connection& c, const Message& m) {
  const std::string& room = m.field(0);
  if (!valid_room(room)) return send(c, error(m.type, "room must be 1-32 letters, digits, '_' or '-'"));
  std::optional<uint64_t> since;
  if (m.fields.size() > 1) {
    since = parse_u64(m.fields[1]);
    if (!since) return send(c, error(m.type, "bad sequence number"));
  }
  auto result = rooms_.join(room, c.user);
  if (result == RoomManager::JoinResult::TooManyRooms) {
    return send(c, error(m.type, "room limit reached"));
  }
  if (result == RoomManager::JoinResult::Joined) {
    broadcast(room, {MsgType::RoomEvent, {room, "join", c.user}}, c.user);
  }
  send(c, ok(MsgType::Join, room));
  for (const auto& stored : history_for(room, since)) send(c, room_message(room, stored));
  send(c, {MsgType::SyncDone, {room, std::to_string(rooms_.last_seq(room))}});
}

// A rejoin with `since` replays what the client missed, up to max_replay.
// Memory holds the newest history_limit messages; larger gaps come from SQLite.
std::vector<StoredMessage> Server::history_for(const std::string& room,
                                               std::optional<uint64_t> since) {
  if (!since) return rooms_.history_since(room, 0, kJoinHistory);
  uint64_t oldest = rooms_.oldest_seq(room);
  bool memory_covers = oldest != 0 && oldest <= *since + 1;
  if (storage_ && !memory_covers && *since < rooms_.last_seq(room)) {
    return storage_->history_since(room, *since, config_.max_replay);
  }
  return rooms_.history_since(room, *since, config_.max_replay);
}

void Server::on_leave(Connection& c, const Message& m) {
  const std::string& room = m.field(0);
  if (!rooms_.leave(room, c.user)) return send(c, error(m.type, "not in room " + room));
  broadcast(room, {MsgType::RoomEvent, {room, "leave", c.user}});
  send(c, ok(MsgType::Leave, room));
}

void Server::on_say(Connection& c, const Message& m) {
  const std::string& room = m.field(0);
  const std::string& text = m.field(1);
  if (!valid_text(text)) return send(c, error(m.type, "text must be 1-4096 printable bytes"));
  if (!rooms_.is_member(room, c.user)) return send(c, error(m.type, "not in room " + room));
  const StoredMessage& stored = rooms_.post(room, c.user, text, now_ms());
  if (storage_) storage_->append_message(room, stored);
  broadcast(room, room_message(room, stored));
}

void Server::on_who(Connection& c, const Message& m) {
  const std::string& room = m.field(0);
  Message reply{MsgType::Members, {room}};
  for (auto& member : rooms_.members(room)) reply.fields.push_back(std::move(member));
  send(c, reply);
}

void Server::on_list_rooms(Connection& c) { send(c, {MsgType::Rooms, rooms_.rooms()}); }

// Hybrid step: the server only introduces two peers. It sends each the
// other's UDP endpoint and a fresh shared key; the messages then go direct.
void Server::on_peer_request(Connection& c, const Message& m) {
  const std::string& target = m.field(0);
  auto fail = [&](const std::string& reason) {
    send(c, {MsgType::Error, {to_string(m.type), reason, target}});
  };
  if (target == c.user) return fail("cannot message yourself");
  Session* me = sessions_.find_user(c.user);
  Session* peer = sessions_.find_user(target);
  Connection* peer_conn = connection_of(target);
  if (!peer || !peer_conn) return fail(target + " is not online");
  if (!me || !me->udp) return fail("your UDP endpoint is not registered yet");
  if (!peer->udp) return fail(target + " has no UDP endpoint");

  const std::string key = crypto::to_hex(crypto::random_bytes(crypto::kKeySize));
  const net::Endpoint mine = *me->udp;
  const net::Endpoint theirs = *peer->udp;
  send(c, {MsgType::PeerInfo, {target, theirs.ip, std::to_string(theirs.port), key}});
  send(*peer_conn, {MsgType::PeerInfo, {c.user, mine.ip, std::to_string(mine.port), key}});
  BUZZ_LOG(Debug, "introduced " << c.user << " and " << target);
}

void Server::leave_all_rooms(const std::string& user) {
  for (const auto& room : rooms_.leave_all(user)) {
    broadcast(room, {MsgType::RoomEvent, {room, "leave", user}});
  }
}

// Encodes once and shares the frame with every member.
void Server::broadcast(const std::string& room, const Message& m, const std::string& skip_user) {
  const std::string frame = encode_frame(m);
  for (const auto& member : rooms_.members(room)) {
    if (member == skip_user) continue;
    if (Connection* target = connection_of(member)) send_frame(*target, frame);
  }
}

void Server::read_udp() {
  char buffer[2048];
  for (;;) {
    sockaddr_storage address{};
    socklen_t length = sizeof address;
    ssize_t n = ::recvfrom(udp_fd_.get(), buffer, sizeof buffer, 0,
                           reinterpret_cast<sockaddr*>(&address), &length);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    try {
      Message m = decode_body(std::string_view(buffer, static_cast<size_t>(n)));
      if (m.type == MsgType::UdpHello) on_udp_hello(m, net::endpoint_from(address));
    } catch (const ProtocolError&) {
      // Ignore stray datagrams.
    }
  }
}

// Hello: user | timestamp_ms | hex(HMAC(token, user|timestamp)).
// Timestamps must increase, so a captured hello cannot be replayed.
void Server::on_udp_hello(const Message& m, const net::Endpoint& from) {
  Session* session = sessions_.find_user(m.field(0));
  auto timestamp = parse_i64(m.field(1));
  if (!session || session->conn_id == 0 || !timestamp) return;
  if (*timestamp <= session->last_udp_timestamp) return;
  std::string expected = p2p::hello_mac(session->token, session->user, m.field(1));
  if (!crypto::constant_time_equal(expected, m.field(2))) return;

  session->last_udp_timestamp = *timestamp;
  session->udp = from;
  sockaddr_storage address{};
  socklen_t length = 0;
  if (!net::to_sockaddr(from, address, length)) return;
  std::string ack = encode_body({MsgType::UdpAck, {session->user}});
  ::sendto(udp_fd_.get(), ack.data(), ack.size(), 0, reinterpret_cast<sockaddr*>(&address), length);
}

void Server::expire_sessions() {
  for (const auto& session : sessions_.expire(SessionManager::Clock::now())) {
    BUZZ_LOG(Info, "session of " << session.user << " expired");
  }
}

}  // namespace buzz
