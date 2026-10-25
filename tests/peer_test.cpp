// End-to-end tests of the hybrid path: signed UDP registration, peer
// introduction with consent and X25519 key agreement, then an encrypted
// datagram sent directly between the peers.
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>

#include <memory>

#include "buzz/crypto.hpp"
#include "buzz/p2p.hpp"
#include "support/test_client.hpp"
#include "support/test_server.hpp"

namespace buzz::test {
namespace {

struct Peer {
  std::string name;
  std::string token;
  std::string nonce;
  uint64_t counter = 0;
  std::unique_ptr<TestClient> tcp;
  net::UniqueFd udp = net::udp_bind(0);
  std::unique_ptr<crypto::X25519KeyPair> keys = std::make_unique<crypto::X25519KeyPair>();

  std::string public_hex() const { return crypto::to_hex(keys->public_key()); }
};

std::optional<std::string> receive_datagram(int fd, std::chrono::milliseconds timeout) {
  pollfd pfd{fd, POLLIN, 0};
  if (::poll(&pfd, 1, static_cast<int>(timeout.count())) != 1) return std::nullopt;
  char buffer[2048];
  ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
  if (n <= 0) return std::nullopt;
  return std::string(buffer, static_cast<size_t>(n));
}

void send_datagram(int fd, uint16_t port, const std::string& data) {
  sockaddr_storage address{};
  socklen_t length = 0;
  ASSERT_TRUE(net::to_sockaddr({"127.0.0.1", port}, address, length));
  ::sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&address), length);
}

std::string hello(const Peer& peer, uint64_t counter, const std::string& nonce,
                  bool forge = false) {
  std::string key = crypto::sha256(forge ? "wrong-token" : peer.token);
  std::string text = std::to_string(counter);
  return encode_body(
      {MsgType::UdpHello, {peer.name, text, p2p::hello_mac(key, peer.name, nonce, text)}});
}

class PeerTest : public ::testing::Test {
 protected:
  Peer login(const std::string& name) {
    Peer peer;
    peer.name = name;
    peer.tcp = std::make_unique<TestClient>(server_.cert_file(), server_.port());
    peer.token = peer.tcp->sign_up(name);
    peer.nonce = peer.tcp->welcome().field(3);
    return peer;
  }

  bool register_udp(Peer& peer) {
    send_datagram(peer.udp.get(), server_.udp_port(), hello(peer, ++peer.counter, peer.nonce));
    auto ack = receive_datagram(peer.udp.get(), 5s);
    return ack && decode_body(*ack).type == MsgType::UdpAck;
  }

  TestServer server_;
};

TEST_F(PeerTest, PeersAgreeOnAKeyTheServerNeverSees) {
  Peer alice = login("alice");
  Peer bob = login("bob");
  ASSERT_TRUE(register_udp(alice));
  ASSERT_TRUE(register_udp(bob));

  alice.tcp->send({MsgType::PeerRequest, {"bob", alice.public_hex()}});
  auto offer = bob.tcp->expect(MsgType::PeerOffer);
  ASSERT_TRUE(offer.has_value());
  EXPECT_EQ(offer->field(0), "alice");
  EXPECT_EQ(offer->field(3), alice.public_hex());  // Only public keys travel.

  bob.tcp->send({MsgType::PeerAccept, {"alice", bob.public_hex()}});
  auto info = alice.tcp->expect(MsgType::PeerInfo);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->field(0), "bob");
  EXPECT_EQ(info->field(2), std::to_string(net::local_port(bob.udp.get())));
  EXPECT_EQ(info->field(3), bob.public_hex());

  auto alice_key = p2p::derive_session_key(*alice.keys, "alice", "bob", bob.keys->public_key());
  auto bob_key = p2p::derive_session_key(*bob.keys, "bob", "alice", alice.keys->public_key());
  ASSERT_TRUE(alice_key && bob_key);
  EXPECT_EQ(*alice_key, *bob_key);

  auto bob_port = static_cast<uint16_t>(std::stoi(info->field(2)));
  send_datagram(alice.udp.get(), bob_port,
                p2p::seal_datagram("alice", *alice_key, {MsgType::DmData, {"alice", "1", "psst"}}));
  auto datagram = receive_datagram(bob.udp.get(), 5s);
  ASSERT_TRUE(datagram.has_value());
  auto message = p2p::open_datagram(*datagram, *bob_key);
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->field(2), "psst");
}

// Bug 4: the target's address is shared only if the target accepts.
TEST_F(PeerTest, DeclinedRequestRevealsNothing) {
  Peer alice = login("alice");
  Peer bob = login("bob");
  ASSERT_TRUE(register_udp(alice));
  ASSERT_TRUE(register_udp(bob));
  alice.tcp->send({MsgType::PeerRequest, {"bob", alice.public_hex()}});
  ASSERT_TRUE(bob.tcp->expect(MsgType::PeerOffer).has_value());
  bob.tcp->send({MsgType::PeerDecline, {"alice"}});
  auto reply = alice.tcp->expect(MsgType::Error);
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->field(0), "peer");
  EXPECT_EQ(reply->field(2), "bob");
  EXPECT_FALSE(alice.tcp->expect(MsgType::PeerInfo, 300ms).has_value());
}

TEST_F(PeerTest, UnsolicitedAcceptIsIgnored) {
  Peer alice = login("alice");
  Peer bob = login("bob");
  ASSERT_TRUE(register_udp(alice));
  ASSERT_TRUE(register_udp(bob));
  bob.tcp->send({MsgType::PeerAccept, {"alice", bob.public_hex()}});
  EXPECT_FALSE(alice.tcp->expect(MsgType::PeerInfo, 300ms).has_value());
}

TEST_F(PeerTest, ForgedHelloIsIgnored) {
  Peer alice = login("alice");
  send_datagram(alice.udp.get(), server_.udp_port(), hello(alice, 1, alice.nonce, /*forge=*/true));
  EXPECT_FALSE(receive_datagram(alice.udp.get(), 300ms).has_value());
}

TEST_F(PeerTest, ReplayedHelloIsIgnored) {
  Peer alice = login("alice");
  ASSERT_TRUE(register_udp(alice));
  send_datagram(alice.udp.get(), server_.udp_port(), hello(alice, alice.counter, alice.nonce));
  EXPECT_FALSE(receive_datagram(alice.udp.get(), 300ms).has_value());
}

// Bug 9: registration no longer depends on the client's clock. After a
// resume the counter starts again, and hellos from before are rejected.
TEST_F(PeerTest, ResumeStartsAFreshHelloEpoch) {
  Peer alice = login("alice");
  ASSERT_TRUE(register_udp(alice));
  std::string old_hello = hello(alice, 50, alice.nonce);

  TestClient again(server_.cert_file(), server_.port());
  again.send({MsgType::Resume, {alice.token}});
  auto welcome = again.expect(MsgType::Welcome);
  ASSERT_TRUE(welcome.has_value());
  EXPECT_NE(welcome->field(3), alice.nonce);

  send_datagram(alice.udp.get(), server_.udp_port(), old_hello);
  EXPECT_FALSE(receive_datagram(alice.udp.get(), 300ms).has_value());
  alice.nonce = welcome->field(3);
  alice.counter = 0;
  EXPECT_TRUE(register_udp(alice));
}

TEST_F(PeerTest, PeerRequestNeedsBothEndpoints) {
  Peer alice = login("alice");
  Peer bob = login("bob");
  alice.tcp->send({MsgType::PeerRequest, {"bob", alice.public_hex()}});
  auto reply = alice.tcp->expect(MsgType::Error);
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->field(2), "bob");

  alice.tcp->send({MsgType::PeerRequest, {"nobody", alice.public_hex()}});
  reply = alice.tcp->expect(MsgType::Error);
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->field(1), "nobody is not online");
}

}  // namespace
}  // namespace buzz::test
