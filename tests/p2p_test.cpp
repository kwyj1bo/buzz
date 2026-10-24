#include <gtest/gtest.h>

#include "buzz/crypto.hpp"
#include "buzz/p2p.hpp"

namespace buzz::p2p {
namespace {

const std::string kKey(crypto::kKeySize, 'k');

TEST(P2P, DatagramRoundTrip) {
  Message dm{MsgType::DmData, {"alice", "1", "hi bob"}};
  std::string datagram = seal_datagram("alice", kKey, dm);
  EXPECT_EQ(peek_sender(datagram), "alice");
  auto opened = open_datagram(datagram, kKey);
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(opened->type, MsgType::DmData);
  EXPECT_EQ(opened->fields, dm.fields);
}

TEST(P2P, WrongKeyFails) {
  std::string datagram = seal_datagram("alice", kKey, {MsgType::DmPunch, {"alice"}});
  EXPECT_FALSE(open_datagram(datagram, std::string(crypto::kKeySize, 'x')).has_value());
}

TEST(P2P, SenderNameIsAuthenticated) {
  std::string datagram = seal_datagram("alice", kKey, {MsgType::DmPunch, {"alice"}});
  datagram[1] = 'A';  // Spoof the clear sender name.
  EXPECT_FALSE(open_datagram(datagram, kKey).has_value());
}

TEST(P2P, GarbageIsRejected) {
  EXPECT_FALSE(peek_sender("").has_value());
  EXPECT_FALSE(peek_sender(std::string("\x05" "ab", 3)).has_value());
  EXPECT_FALSE(open_datagram("\x01" "a garbage", kKey).has_value());
}

TEST(P2P, HelloMacDependsOnEveryInput) {
  std::string mac = hello_mac("token", "alice", "1000");
  EXPECT_EQ(mac, hello_mac("token", "alice", "1000"));
  EXPECT_NE(mac, hello_mac("token2", "alice", "1000"));
  EXPECT_NE(mac, hello_mac("token", "bob", "1000"));
  EXPECT_NE(mac, hello_mac("token", "alice", "1001"));
}

}  // namespace
}  // namespace buzz::p2p
