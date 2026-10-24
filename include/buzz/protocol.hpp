#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace buzz {

// Wire format of one message body:
//   u8 type | u16 field_count | (u32 length | bytes) * field_count
// On TCP each body is prefixed with a u32 length (a frame).
// On UDP each datagram holds exactly one body. All integers are big-endian.
enum class MsgType : uint8_t {
  // Client to server.
  Register = 1,
  Login = 2,
  Resume = 3,
  Logout = 4,
  Join = 5,
  Leave = 6,
  Say = 7,
  ListRooms = 8,
  Who = 9,
  PeerRequest = 10,
  Ping = 11,
  Relay = 12,  // Direct message through the server when P2P fails.

  // Server to client.
  Ok = 64,
  Error = 65,
  Welcome = 66,
  RoomMsg = 67,
  RoomEvent = 68,
  Rooms = 69,
  Members = 70,
  PeerInfo = 71,
  Pong = 72,
  SyncDone = 73,
  Relayed = 74,

  // UDP: client to server registration, and peer to peer.
  UdpHello = 100,
  UdpAck = 101,
  DmData = 102,
  DmAck = 103,
  DmPunch = 104,
};

constexpr size_t kMaxFrameSize = 64 * 1024;
constexpr size_t kMaxFields = 256;

class ProtocolError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct Message {
  MsgType type{};
  std::vector<std::string> fields;

  // Throws ProtocolError when the field is missing.
  const std::string& field(size_t index) const;
};

const char* to_string(MsgType type);
bool is_known_type(uint8_t raw);

std::string encode_body(const Message& message);
Message decode_body(std::string_view body);
std::string encode_frame(const Message& message);

// Splits a TCP byte stream into messages.
class FrameDecoder {
 public:
  explicit FrameDecoder(size_t max_frame = kMaxFrameSize);

  void feed(const char* data, size_t size);
  // Returns the next complete message. Throws ProtocolError on a bad frame.
  std::optional<Message> next();
  size_t buffered() const { return buffer_.size() - offset_; }

 private:
  void compact();

  std::string buffer_;
  size_t offset_ = 0;
  size_t max_frame_;
};

}  // namespace buzz
