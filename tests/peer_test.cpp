// End-to-end test of the hybrid path: UDP registration with the server,
// peer introduction over TLS, then an encrypted datagram sent directly.
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
  std::unique_ptr<TestClient> tcp;
  net::UniqueFd udp = net::udp_bind(0);
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

std::string hello(const Peer& peer, const std::string& timestamp, bool forge = false) {
  std::string mac = p2p::hello_mac(forge ? "wrong-token" : peer.token, peer.name, timestamp);
  return encode_body({MsgType::UdpHello, {peer.name, timestamp, mac}});
}

class PeerTest : public ::testing::Test {
 protected:
  Peer login(const std::string& name) {
    Peer peer;
    peer.name = name;
    peer.tcp = std::make_unique<TestClient>(server_.cert_file(), server_.port());
    peer.token = peer.tcp->sign_up(name);
    return peer;
  }

  void register_udp(const Peer& peer) {
    send_datagram(peer.udp.get(), server_.udp_port(), hello(peer, "1000"));
    auto ack = receive_datagram(peer.udp.get(), 5s);
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(decode_body(*ack).type, MsgType::UdpAck);
  }

  TestServer server_;
};

TEST_F(PeerTest, IntroducesPeersWhoThenTalkDirectly) {
  Peer alice = login("alice");
  Peer bob = login("bob");
  register_udp(alice);
  register_udp(bob);

  alice.tcp->send({MsgType::PeerRequest, {"bob"}});
  auto to_alice = alice.tcp->expect(MsgType::PeerInfo);
  auto to_bob = bob.tcp->expect(MsgType::PeerInfo);
  ASSERT_TRUE(to_alice && to_bob);
  EXPECT_EQ(to_alice->field(0), "bob");
  EXPECT_EQ(to_bob->field(0), "alice");
  EXPECT_EQ(to_alice->field(2), std::to_string(net::local_port(bob.udp.get())));
  EXPECT_EQ(to_alice->field(3), to_bob->field(3));  // Shared key.

  auto key = crypto::from_hex(to_alice->field(3));
  ASSERT_TRUE(key.has_value());
  auto bob_port = static_cast<uint16_t>(std::stoi(to_alice->field(2)));
  send_datagram(alice.udp.get(), bob_port,
                p2p::seal_datagram("alice", *key, {MsgType::DmData, {"alice", "1", "psst"}}));

  auto datagram = receive_datagram(bob.udp.get(), 5s);
  ASSERT_TRUE(datagram.has_value());
  auto bob_key = crypto::from_hex(to_bob->field(3));
  auto message = p2p::open_datagram(*datagram, *bob_key);
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->field(2), "psst");
}

TEST_F(PeerTest, ForgedHelloIsIgnored) {
  Peer alice = login("alice");
  send_datagram(alice.udp.get(), server_.udp_port(), hello(alice, "1000", /*forge=*/true));
  EXPECT_FALSE(receive_datagram(alice.udp.get(), 300ms).has_value());
}

TEST_F(PeerTest, ReplayedHelloIsIgnored) {
  Peer alice = login("alice");
  register_udp(alice);
  send_datagram(alice.udp.get(), server_.udp_port(), hello(alice, "1000"));
  EXPECT_FALSE(receive_datagram(alice.udp.get(), 300ms).has_value());
}

TEST_F(PeerTest, PeerRequestNeedsBothEndpoints) {
  Peer alice = login("alice");
  Peer bob = login("bob");
  alice.tcp->send({MsgType::PeerRequest, {"bob"}});
  auto reply = alice.tcp->expect(MsgType::Error);
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->field(2), "bob");

  alice.tcp->send({MsgType::PeerRequest, {"nobody"}});
  reply = alice.tcp->expect(MsgType::Error);
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->field(1), "nobody is not online");
}

}  // namespace
}  // namespace buzz::test
