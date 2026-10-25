// Rooms, room chat and history sync.
#include "buzz/log.hpp"
#include "buzz/server.hpp"
#include "buzz/validate.hpp"
#include "connection.hpp"
#include "messages.hpp"

namespace buzz {
namespace {

constexpr size_t kJoinHistory = 50;
// History is queued only while the output buffer is below this, so a large
// replay never trips max_output_buffer and disconnects the client.
constexpr size_t kReplayHighWater = 256 * 1024;
constexpr size_t kMaxPendingReplays = 8;

}  // namespace

void Server::on_join(Connection& c, const Message& m) {
  const std::string& room = m.field(0);
  if (!valid_room(room)) return send(c, error(m.type, "room must be 1-32 letters, digits, '_' or '-'"));
  std::optional<uint64_t> since;
  if (m.fields.size() > 1) {
    since = parse_u64(m.fields[1]);
    if (!since) return send(c, error(m.type, "bad sequence number"));
  }
  if (c.replays.size() >= kMaxPendingReplays) {
    return send(c, error(m.type, "too many joins in progress"));
  }
  if (!load_room(room)) return send(c, error(m.type, "room limit reached, try again later"));
  auto result = rooms_.join(room, c.user);
  if (result == RoomManager::JoinResult::TooManyRooms) {
    return send(c, error(m.type, "room limit reached, try again later"));
  }
  if (result == RoomManager::JoinResult::UserRoomLimit) {
    return send(c, error(m.type, "you are in too many rooms; leave one first"));
  }
  if (result == RoomManager::JoinResult::Joined) {
    broadcast(room, {MsgType::RoomEvent, {room, "join", c.user}}, c.user);
  }
  send(c, ok(MsgType::Join, room));
  auto history = history_for(room, since);
  c.replays.push_back({room, {history.begin(), history.end()}});
  pump_replays(c);
}

// Loads a room's sequence number and recent history from SQLite the first
// time it is used. Returns false when no room slot is free.
bool Server::load_room(const std::string& room) {
  if (rooms_.exists(room) || !storage_) return true;
  std::optional<Storage::RoomHistory> history;
  try {
    history = storage_->load_room(room, config_.history_limit);
  } catch (const StorageError& e) {
    BUZZ_LOG(Error, "cannot load room " << room << ": " << e.what());
  }
  if (!history) return true;  // A new room; join creates it.
  return rooms_.restore(room, history->last_seq, std::move(history->recent));
}

// A rejoin with `since` replays what the client missed, up to max_replay.
// Memory holds the newest history_limit messages; larger gaps come from SQLite.
std::vector<StoredMessage> Server::history_for(const std::string& room,
                                               std::optional<uint64_t> since) {
  if (!since) return rooms_.history_since(room, 0, kJoinHistory);
  uint64_t oldest = rooms_.oldest_seq(room);
  bool memory_covers = oldest != 0 && oldest <= *since + 1;
  if (storage_ && !memory_covers && *since < rooms_.last_seq(room)) {
    try {
      return storage_->history_since(room, *since, config_.max_replay);
    } catch (const StorageError& e) {
      BUZZ_LOG(Error, "cannot read history of " << room << ": " << e.what());
    }
  }
  return rooms_.history_since(room, *since, config_.max_replay);
}

// Sends owed history while the output buffer has room. Called again as the
// socket drains. SyncDone carries the room's newest seq when the replay ends;
// live messages sent meanwhile are already ahead of it on the stream.
void Server::pump_replays(Connection& c) {
  while (!c.replays.empty() && !c.closing && c.out.size() < kReplayHighWater) {
    auto& replay = c.replays.front();
    if (replay.messages.empty()) {
      send(c, {MsgType::SyncDone, {replay.room, std::to_string(rooms_.last_seq(replay.room))}});
      c.replays.pop_front();
      continue;
    }
    send(c, room_message(replay.room, replay.messages.front()));
    replay.messages.pop_front();
  }
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

}  // namespace buzz
