// Message dispatch, accounts and sessions.
#include <chrono>

#include "buzz/crypto.hpp"
#include "buzz/log.hpp"
#include "buzz/server.hpp"
#include "buzz/validate.hpp"
#include "connection.hpp"
#include "messages.hpp"

namespace buzz {

void Server::handle(Connection& c, const Message& m) {
  switch (m.type) {
    case MsgType::Register: return on_register(c, m);
    case MsgType::Login: return on_login(c, m);
    case MsgType::Resume: return on_resume(c, m);
    case MsgType::Ping: return send(c, {MsgType::Pong, m.fields});
    default: break;
  }
  if (c.user.empty()) return send(c, error(m.type, "not logged in"));
  if (!within_rate(c, m.type)) return send(c, error(m.type, "rate limit exceeded, slow down"));
  switch (m.type) {
    case MsgType::Logout: return on_logout(c);
    case MsgType::Join: return on_join(c, m);
    case MsgType::Leave: return on_leave(c, m);
    case MsgType::Say: return on_say(c, m);
    case MsgType::Who: return on_who(c, m);
    case MsgType::ListRooms: return on_list_rooms(c);
    case MsgType::PeerRequest: return on_peer_request(c, m);
    case MsgType::PeerAccept: return on_peer_accept(c, m);
    case MsgType::PeerDecline: return on_peer_decline(c, m);
    case MsgType::Relay: return on_relay(c, m);
    default: return send(c, error(m.type, "unexpected message"));
  }
}

// Register and Login each cost a PBKDF2 hash, so they draw from a budget
// per IP address as well as the per-connection limits.
bool Server::admit_auth(Connection& c, MsgType type) {
  if (c.auth_pending) {
    send(c, error(type, "authentication already in progress"));
    return false;
  }
  if (!throttle_.allow(c.ip, Clock::now())) {
    send(c, error(type, "too many attempts from your address, try again later"));
    return false;
  }
  return true;
}

void Server::on_register(Connection& c, const Message& m) {
  std::string user = m.field(0);
  std::string password = m.field(1);
  if (!valid_username(user)) {
    return send(c, error(m.type, "username must be 3-32 letters, digits, '_' or '-'"));
  }
  if (!valid_password(password)) {
    return send(c, error(m.type, "password must be 8-128 characters"));
  }
  if (!admit_auth(c, m.type)) return;
  c.auth_pending = true;
  pool_.submit([this, id = c.id, user = std::move(user), password = std::move(password)]() mutable {
    AuthResult result{id, MsgType::Register, user, false, "server error, try again"};
    try {
      auto added = users_.add_user(user, password);
      result.ok = added == UserStore::RegisterResult::Ok;
      if (!result.ok) result.error = "username is taken";
    } catch (const std::exception& e) {
      BUZZ_LOG(Error, "register failed: " << e.what());
    }
    crypto::wipe(password);
    post_result(std::move(result));
  });
}

void Server::on_login(Connection& c, const Message& m) {
  std::string user = m.field(0);
  std::string password = m.field(1);
  auto locked = throttle_.locked_for(user, Clock::now());
  if (locked > Clock::duration::zero()) {
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(locked).count() + 1;
    return send(c, error(m.type, "too many failed logins for " + user + ", try again in " +
                                     std::to_string(seconds) + "s"));
  }
  if (!admit_auth(c, m.type)) return;
  c.auth_pending = true;
  pool_.submit([this, id = c.id, user = std::move(user), password = std::move(password)]() mutable {
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
    // Failures count per username, so reconnecting does not reset them.
    if (r.kind == MsgType::Login && r.ok) throttle_.record_success(r.user);
    if (r.kind == MsgType::Login && !r.ok) throttle_.record_failure(r.user, Clock::now());
    Connection* c = find_connection(r.conn_id);
    if (!c || c->closing) continue;
    c->auth_pending = false;
    on_auth_result(*c, r);
  }
}

void Server::on_auth_result(Connection& c, const AuthResult& r) {
  if (r.ok && r.kind == MsgType::Register) {
    BUZZ_LOG(Info, "registered " << r.user);
    return send(c, ok(MsgType::Register, r.user));
  }
  if (r.ok) return start_session(c, r.user);
  send(c, error(r.kind, r.error));
  if (r.kind == MsgType::Login && ++c.failed_logins >= config_.max_failed_logins) {
    BUZZ_LOG(Warn, "#" << c.id << " closed after " << c.failed_logins << " failed logins");
    mark_close(c);
  }
}

void Server::start_session(Connection& c, const std::string& user) {
  if (!c.user.empty()) end_session(c);
  if (auto old = sessions_.remove_user(user)) {
    take_over_session(*old, c.id, "logged in from another connection");
  }
  std::string token = sessions_.create(user, c.id);
  c.user = user;
  c.ever_logged_in = true;
  BUZZ_LOG(Info, user << " logged in from " << c.peer);
  welcome(c, token);
}

void Server::on_resume(Connection& c, const Message& m) {
  const std::string& token = m.field(0);
  Session* session = sessions_.find_token(token);
  if (!session) return send(c, error(m.type, "session expired, log in again"));
  const std::string user = session->user;
  if (c.user != user) {
    if (!c.user.empty()) end_session(c);
    take_over_session(*sessions_.find_user(user), c.id, "session resumed elsewhere");
    c.user = user;
    c.ever_logged_in = true;
  }
  sessions_.attach(user, c.id);
  BUZZ_LOG(Info, user << " resumed from " << c.peer);
  welcome(c, token);
}

// Welcome: user | token | udp port | nonce for signing UDP hellos.
void Server::welcome(Connection& c, const std::string& token) {
  const Session* session = sessions_.find_user(c.user);
  send(c, {MsgType::Welcome, {c.user, token, std::to_string(udp_port_), session->udp_nonce}});
}

// Moves a user from an old connection to a new one. The old connection is
// told why and closed; the user leaves its rooms so membership starts clean.
void Server::take_over_session(const Session& old, uint64_t new_conn_id, const char* reason) {
  if (old.conn_id == 0 || old.conn_id == new_conn_id) return;
  leave_all_rooms(old.user);
  if (Connection* previous = find_connection(old.conn_id)) {
    send(*previous, {MsgType::Error, {"session", reason}});
    previous->user.clear();
    previous->replays.clear();
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
  c.replays.clear();
}

// The session stays for session_ttl so the client can resume it.
void Server::detach_session(Connection& c) {
  if (c.user.empty()) return;
  Session* session = sessions_.find_user(c.user);
  if (session && session->conn_id == c.id) {
    sessions_.detach(c.user, Clock::now());
    leave_all_rooms(c.user);
  }
  c.user.clear();
}

// Every request from a logged-in client shares one token bucket, so no
// request type can make the server do unbounded work.
bool Server::within_rate(Connection& c, MsgType type) {
  return type == MsgType::Logout || c.limiter.take();
}

void Server::expire_sessions() {
  for (const auto& session : sessions_.expire(Clock::now())) {
    BUZZ_LOG(Info, "session of " << session.user << " expired");
  }
}

}  // namespace buzz
