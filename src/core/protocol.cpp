#include "buzz/protocol.hpp"

namespace buzz {
namespace {

constexpr size_t kLengthSize = 4;
constexpr size_t kBodyHeaderSize = 3;

void put_u16(std::string& out, uint16_t value) {
  out.push_back(static_cast<char>(value >> 8));
  out.push_back(static_cast<char>(value & 0xff));
}

void put_u32(std::string& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

uint16_t get_u16(const char* p) {
  return static_cast<uint16_t>(static_cast<uint8_t>(p[0]) << 8 | static_cast<uint8_t>(p[1]));
}

uint32_t get_u32(const char* p) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value = value << 8 | static_cast<uint8_t>(p[i]);
  return value;
}

}  // namespace

const std::string& Message::field(size_t index) const {
  if (index >= fields.size()) {
    throw ProtocolError(std::string("missing field in ") + to_string(type));
  }
  return fields[index];
}

std::string encode_body(const Message& message) {
  if (message.fields.size() > kMaxFields) throw ProtocolError("too many fields");
  size_t total = kBodyHeaderSize;
  for (const auto& f : message.fields) total += kLengthSize + f.size();

  std::string out;
  out.reserve(total);
  out.push_back(static_cast<char>(message.type));
  put_u16(out, static_cast<uint16_t>(message.fields.size()));
  for (const auto& f : message.fields) {
    put_u32(out, static_cast<uint32_t>(f.size()));
    out.append(f);
  }
  return out;
}

Message decode_body(std::string_view body) {
  if (body.size() < kBodyHeaderSize) throw ProtocolError("truncated message header");
  auto raw = static_cast<uint8_t>(body[0]);
  if (!is_known_type(raw)) throw ProtocolError("unknown message type");
  uint16_t count = get_u16(body.data() + 1);
  if (count > kMaxFields) throw ProtocolError("too many fields");

  Message message{static_cast<MsgType>(raw), {}};
  message.fields.reserve(count);
  size_t pos = kBodyHeaderSize;
  for (uint16_t i = 0; i < count; ++i) {
    if (body.size() - pos < kLengthSize) throw ProtocolError("truncated field length");
    uint32_t length = get_u32(body.data() + pos);
    pos += kLengthSize;
    if (length > body.size() - pos) throw ProtocolError("truncated field");
    message.fields.emplace_back(body.substr(pos, length));
    pos += length;
  }
  if (pos != body.size()) throw ProtocolError("trailing bytes after message");
  return message;
}

std::string encode_frame(const Message& message) {
  std::string body = encode_body(message);
  if (body.size() > kMaxFrameSize) throw ProtocolError("frame too large");
  std::string out;
  out.reserve(kLengthSize + body.size());
  put_u32(out, static_cast<uint32_t>(body.size()));
  out += body;
  return out;
}

FrameDecoder::FrameDecoder(size_t max_frame) : max_frame_(max_frame) {}

void FrameDecoder::feed(const char* data, size_t size) { buffer_.append(data, size); }

std::optional<Message> FrameDecoder::next() {
  size_t available = buffered();
  if (available < kLengthSize) {
    compact();
    return std::nullopt;
  }
  uint32_t length = get_u32(buffer_.data() + offset_);
  if (length > max_frame_) throw ProtocolError("frame exceeds maximum size");
  if (available - kLengthSize < length) {
    compact();
    return std::nullopt;
  }
  Message message = decode_body(std::string_view(buffer_).substr(offset_ + kLengthSize, length));
  offset_ += kLengthSize + length;
  return message;
}

void FrameDecoder::compact() {
  if (offset_ == 0) return;
  buffer_.erase(0, offset_);
  offset_ = 0;
}

const char* to_string(MsgType type) {
  switch (type) {
    case MsgType::Register: return "register";
    case MsgType::Login: return "login";
    case MsgType::Resume: return "resume";
    case MsgType::Logout: return "logout";
    case MsgType::Join: return "join";
    case MsgType::Leave: return "leave";
    case MsgType::Say: return "say";
    case MsgType::ListRooms: return "rooms";
    case MsgType::Who: return "who";
    case MsgType::PeerRequest: return "peer";
    case MsgType::Ping: return "ping";
    case MsgType::Relay: return "relay";
    case MsgType::PeerAccept: return "peer_accept";
    case MsgType::PeerDecline: return "peer_decline";
    case MsgType::Ok: return "ok";
    case MsgType::Error: return "error";
    case MsgType::Welcome: return "welcome";
    case MsgType::RoomMsg: return "room_msg";
    case MsgType::RoomEvent: return "room_event";
    case MsgType::Rooms: return "room_list";
    case MsgType::Members: return "members";
    case MsgType::PeerInfo: return "peer_info";
    case MsgType::Pong: return "pong";
    case MsgType::SyncDone: return "sync_done";
    case MsgType::Relayed: return "relayed";
    case MsgType::PeerOffer: return "peer_offer";
    case MsgType::UdpHello: return "udp_hello";
    case MsgType::UdpAck: return "udp_ack";
    case MsgType::DmData: return "dm_data";
    case MsgType::DmAck: return "dm_ack";
    case MsgType::DmPunch: return "dm_punch";
  }
  return "unknown";
}

bool is_known_type(uint8_t raw) {
  return std::string_view(to_string(static_cast<MsgType>(raw))) != "unknown";
}

}  // namespace buzz
