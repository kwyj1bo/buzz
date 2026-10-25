#pragma once

#include <chrono>
#include <string>

#include "buzz/protocol.hpp"
#include "buzz/room_manager.hpp"

// Small builders shared by the server's handler files.
namespace buzz {

inline Message error(MsgType context, const std::string& reason) {
  return {MsgType::Error, {to_string(context), reason}};
}

inline Message ok(MsgType context, const std::string& detail = "") {
  return {MsgType::Ok, {to_string(context), detail}};
}

inline Message room_message(const std::string& room, const StoredMessage& m) {
  return {MsgType::RoomMsg,
          {room, std::to_string(m.seq), std::to_string(m.timestamp_ms), m.from, m.text}};
}

inline int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace buzz
