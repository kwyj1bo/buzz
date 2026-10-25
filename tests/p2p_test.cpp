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
  std::string mac = hello_mac("key", "alice", "nonce", "1");
  EXPECT_EQ(mac, hello_mac("key", "alice", "nonce", "1"));
  EXPECT_NE(mac, hello_mac("key2", "alice", "nonce", "1"));
  EXPECT_NE(mac, hello_mac("key", "bob", "nonce", "1"));
  EXPECT_NE(mac, hello_mac("key", "alice", "nonce2", "1"));
  EXPECT_NE(mac, hello_mac("key", "alice", "nonce", "2"));
}

// Bug 4: both peers derive the same key from public values only.
TEST(P2P, SessionKeyAgreement) {
  crypto::X25519KeyPair alice;
  crypto::X25519KeyPair bob;
  crypto::X25519KeyPair mallory;
  auto a = derive_session_key(alice, "alice", "bob", bob.public_key());
  auto b = derive_session_key(bob, "bob", "alice", alice.public_key());
  ASSERT_TRUE(a && b);
  EXPECT_EQ(*a, *b);
  EXPECT_EQ(a->size(), crypto::kKeySize);
  // A key swapped in by someone in the middle gives a different key.
  EXPECT_NE(*a, *derive_session_key(alice, "alice", "bob", mallory.public_key()));
  // The key is bound to the names, too.
  EXPECT_NE(*a, *derive_session_key(alice, "alice", "carol", bob.public_key()));
}

TEST(P2P, SessionKeyRejectsBadPublicKeys) {
  crypto::X25519KeyPair alice;
  EXPECT_FALSE(derive_session_key(alice, "alice", "bob", "short").has_value());
  EXPECT_FALSE(derive_session_key(alice, "alice", "bob", std::string(32, '\0')).has_value());
}

TEST(P2P, SafetyNumberIsSymmetricAndDetectsSwaps) {
  crypto::X25519KeyPair alice;
  crypto::X25519KeyPair bob;
  crypto::X25519KeyPair mallory;
  std::string ab = safety_number("alice", alice.public_key(), "bob", bob.public_key());
  EXPECT_EQ(ab, safety_number("bob", bob.public_key(), "alice", alice.public_key()));
  EXPECT_NE(ab, safety_number("alice", alice.public_key(), "bob", mallory.public_key()));
  EXPECT_EQ(ab.size(), 35u);  // Six groups of five.
}

}  // namespace
}  // namespace buzz::p2p
