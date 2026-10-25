#include <unistd.h>

#include <sstream>

#include "buzz/p2p.hpp"
#include "buzz/validate.hpp"
#include "client.hpp"

namespace buzz::client {
namespace {

constexpr const char* kHelp = R"(commands:
  /register <user> <password>   create an account
  /login <user> <password>      log in
  /logout                       end the session
  /join <room>                  join a room and make it current
  /leave [room]                 leave a room (default: current)
  /room <room>                  switch the current room
  /rooms                        list rooms on the server
  /who [room]                   list members of a room
  /dm <user> <text>             send an end-to-end encrypted direct message
  /verify <user>                show the safety number of a direct channel
  /peers                        list direct channels
  /ping                         measure server round trip
  /quit                         exit (the session can resume for a while)
  <text>                        say something in the current room)";

int64_t wall_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::vector<std::string> split(const std::string& text) {
  std::istringstream in(text);
  std::vector<std::string> words;
  for (std::string word; in >> word;) words.push_back(word);
  return words;
}

// Text after the first `count` words, with its own spacing kept.
std::string rest_after(const std::string& line, size_t count) {
  size_t pos = 0;
  for (size_t i = 0; i < count; ++i) {
    pos = line.find_first_not_of(' ', pos);
    pos = line.find(' ', pos);
    if (pos == std::string::npos) return "";
  }
  pos = line.find_first_not_of(' ', pos);
  return pos == std::string::npos ? "" : line.substr(pos);
}

}  // namespace

void Client::submit(std::string line) {
  {
    std::lock_guard lock(lines_mutex_);
    lines_.push_back(std::move(line));
  }
  char byte = 1;
  [[maybe_unused]] ssize_t n = ::write(wake_write_.get(), &byte, 1);
}

void Client::process_lines() {
  char buffer[64];
  while (::read(wake_read_.get(), buffer, sizeof buffer) > 0) {
  }
  std::deque<std::string> lines;
  {
    std::lock_guard lock(lines_mutex_);
    lines.swap(lines_);
  }
  for (const auto& line : lines) {
    if (running_) handle_line(line);
  }
}

void Client::handle_line(const std::string& line) {
  if (line.empty()) return;
  if (line[0] != '/') return say(line);
  auto args = split(line.substr(1));
  if (args.empty()) return;
  const std::string& cmd = args[0];

  if (cmd == "help") return print(kHelp);
  if (cmd == "quit") {
    running_ = false;
    return;
  }
  if (cmd == "join") return cmd_join(args);
  if (cmd == "leave") return cmd_leave(args);
  if (cmd == "room") return cmd_room(args);
  if (cmd == "verify") return cmd_verify(args);
  if (cmd == "dm") {
    if (args.size() < 3) return print("usage: /dm <user> <text>");
    return cmd_dm(args[1], rest_after(line.substr(1), 2));
  }
  if (cmd == "peers") {
    for (const auto& line_out : peers_.describe()) print("  " + line_out);
    return;
  }
  if (cmd == "register" || cmd == "login") {
    if (args.size() != 3) return print("usage: /" + cmd + " <user> <password>");
    MsgType type = cmd == "register" ? MsgType::Register : MsgType::Login;
    if (require_link()) link_.send({type, {args[1], args[2]}});
    return;
  }
  if (cmd == "ping") {
    if (require_link()) link_.send({MsgType::Ping, {std::to_string(wall_ms())}});
    return;
  }
  if (cmd == "logout" || cmd == "rooms") {
    MsgType type = cmd == "logout" ? MsgType::Logout : MsgType::ListRooms;
    if (require_login()) link_.send({type, {}});
    return;
  }
  if (cmd == "who") {
    std::string room = args.size() > 1 ? args[1] : rooms_.current();
    if (room.empty()) return print("usage: /who <room>");
    if (require_login()) link_.send({MsgType::Who, {room}});
    return;
  }
  print("! unknown command /" + cmd + " (try /help)");
}

// The room becomes current only when the server confirms the join.
void Client::cmd_join(const std::vector<std::string>& args) {
  if (args.size() != 2) return print("usage: /join <room>");
  if (!require_login()) return;
  const std::string& room = args[1];
  if (!valid_room(room)) return print("! room names use letters, digits, '_' or '-'");
  rooms_.request_join(room);
  link_.send({MsgType::Join, {room}});
}

void Client::cmd_leave(const std::vector<std::string>& args) {
  std::string room = args.size() > 1 ? args[1] : rooms_.current();
  if (room.empty()) return print("usage: /leave <room>");
  if (require_login()) link_.send({MsgType::Leave, {room}});
}

void Client::cmd_room(const std::vector<std::string>& args) {
  if (args.size() != 2) return print("usage: /room <room>");
  if (!rooms_.set_current(args[1])) return print("! not in #" + args[1] + "; /join it first");
  print("* talking in #" + args[1]);
}

void Client::cmd_dm(const std::string& peer, const std::string& text) {
  if (peer == user_) return print("! cannot message yourself");
  if (!valid_text(text)) return print("! message must be 1-4096 printable characters");
  contacts_.insert(peer);
  if (peers_.has_peer(peer)) return peers_.send_text(peer, text, Clock::now());
  if (!require_login()) return;
  // Introductions need our UDP endpoint; until it is registered the message
  // waits and the request goes out on registration.
  if (dms_.add(peer, text, udp_registered_, Clock::now())) request_peer(peer);
}

void Client::cmd_verify(const std::vector<std::string>& args) {
  if (args.size() != 2) return print("usage: /verify <user>");
  auto theirs = peers_.public_key(args[1]);
  if (!theirs) return print("! no direct channel to " + args[1] + " yet; /dm them first");
  print("safety number with " + args[1] + ": " +
        p2p::safety_number(user_, keys_.public_key(), args[1], *theirs));
  print("  compare it with " + args[1] + " in person or by phone; a mismatch means the keys "
        "were swapped by someone in the middle");
}

void Client::say(const std::string& text) {
  if (!require_login()) return;
  if (rooms_.current().empty()) return print("! join a room first: /join <room>");
  if (!valid_text(text)) return print("! message must be 1-4096 printable characters");
  link_.send({MsgType::Say, {rooms_.current(), text}});
}

// Connects on demand when the client is idle without a session.
bool Client::require_link() {
  if (!link_.connected()) {
    connect_policy_.want_now();
    connect();
  }
  if (link_.connected()) return true;
  print("! not connected to the server");
  return false;
}

bool Client::require_login() {
  if (!require_link()) return false;
  if (!token_.empty()) return true;
  print("! log in first: /login <user> <password>");
  return false;
}

}  // namespace buzz::client
