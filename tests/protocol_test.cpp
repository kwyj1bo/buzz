#include <gtest/gtest.h>

#include "buzz/protocol.hpp"

namespace buzz {
namespace {

Message sample() { return {MsgType::Say, {"general", "hello world"}}; }

TEST(Protocol, BodyRoundTrip) {
  Message decoded = decode_body(encode_body(sample()));
  EXPECT_EQ(decoded.type, MsgType::Say);
  EXPECT_EQ(decoded.fields, sample().fields);
}

TEST(Protocol, KeepsEmptyAndBinaryFields) {
  std::string binary("a\0b\xff", 4);
  Message decoded = decode_body(encode_body({MsgType::Ok, {"", binary}}));
  ASSERT_EQ(decoded.fields.size(), 2u);
  EXPECT_EQ(decoded.fields[0], "");
  EXPECT_EQ(decoded.fields[1], binary);
}

TEST(Protocol, RejectsUnknownType) {
  std::string body = encode_body(sample());
  body[0] = static_cast<char>(200);
  EXPECT_THROW(decode_body(body), ProtocolError);
}

TEST(Protocol, RejectsTruncatedField) {
  std::string body = encode_body(sample());
  body.pop_back();
  EXPECT_THROW(decode_body(body), ProtocolError);
}

TEST(Protocol, RejectsTrailingBytes) {
  EXPECT_THROW(decode_body(encode_body(sample()) + "x"), ProtocolError);
}

TEST(Protocol, RejectsFieldLengthPastEnd) {
  std::string body = encode_body({MsgType::Say, {"abc"}});
  body[3] = '\x7f';  // Huge length on the first field.
  EXPECT_THROW(decode_body(body), ProtocolError);
}

TEST(Protocol, MissingFieldThrows) {
  Message m{MsgType::Join, {}};
  EXPECT_THROW(m.field(0), ProtocolError);
}

TEST(FrameDecoder, ReassemblesByteByByte) {
  std::string frame = encode_frame(sample());
  FrameDecoder decoder;
  for (size_t i = 0; i + 1 < frame.size(); ++i) {
    decoder.feed(&frame[i], 1);
    EXPECT_FALSE(decoder.next().has_value());
  }
  decoder.feed(&frame.back(), 1);
  auto message = decoder.next();
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->fields, sample().fields);
  EXPECT_EQ(decoder.buffered(), 0u);
}

TEST(FrameDecoder, SplitsSeveralFramesInOneRead) {
  std::string stream;
  for (int i = 0; i < 3; ++i) stream += encode_frame({MsgType::Ping, {std::to_string(i)}});
  FrameDecoder decoder;
  decoder.feed(stream.data(), stream.size());
  for (int i = 0; i < 3; ++i) {
    auto message = decoder.next();
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(message->field(0), std::to_string(i));
  }
  EXPECT_FALSE(decoder.next().has_value());
}

TEST(FrameDecoder, RejectsOversizedFrame) {
  FrameDecoder decoder(16);
  std::string frame = encode_frame({MsgType::Say, {"room", std::string(100, 'x')}});
  decoder.feed(frame.data(), frame.size());
  EXPECT_THROW(decoder.next(), ProtocolError);
}

TEST(FrameDecoder, EncoderRefusesHugeFrames) {
  Message big{MsgType::Say, {std::string(kMaxFrameSize, 'x')}};
  EXPECT_THROW(encode_frame(big), ProtocolError);
}

}  // namespace
}  // namespace buzz
