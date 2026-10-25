// Peer introductions, the relay fallback and UDP endpoint registration.
//
// Introduction with consent and end-to-end keys:
//   A -> server: PeerRequest(B, A's X25519 public key)
//   server -> B: PeerOffer(A, A's endpoint, A's public key)
//   B -> server: PeerAccept(A, B's public key)   or   PeerDecline(A)
//   server -> A: PeerInfo(B, B's endpoint, B's public key)   or   Error
// The server forwards public keys only, so it cannot derive the session
// key, and B's address is revealed only if B's client accepts.
#include <sys/socket.h>

#include <cerrno>

#include "buzz/crypto.hpp"
#include "buzz/log.hpp"
#include "buzz/p2p.hpp"
#include "buzz/server.hpp"
#include "buzz/validate.hpp"
#include "connection.hpp"
#include "messages.hpp"

namespace buzz {
namespace {

constexpr auto kOfferTimeout = std::chrono::seconds(60);
constexpr size_t kMaxOffers = 100'000;

bool valid_public_key(const std::string& hex) {
  auto raw = crypto::from_hex(hex);
  return raw && raw->size() == crypto::X25519KeyPair::kPublicSize;
}

}  // namespace

void Server::on_peer_request(Connection& c, const Message& m) {
  const std::string& target = m.field(0);
  const std::string& public_key = m.field(1);
  auto fail = [&](const std::string& reason) {
    send(c, {MsgType::Error, {"peer", reason, target}});
  };
  if (target == c.user) return fail("cannot message yourself");
  if (!valid_public_key(public_key)) return fail("bad public key");
  Session* me = sessions_.find_user(c.user);
  Session* peer = sessions_.find_user(target);
  Connection* peer_conn = connection_of(target);
  if (!peer || !peer_conn) return fail(target + " is not online");
  if (!me || !me->udp) return fail("your UDP endpoint is not registered yet");
  if (!peer->udp) return fail(target + " has no UDP endpoint");
  if (peer_offers_.size() >= kMaxOffers) return fail("server busy, try again");

  peer_offers_[{c.user, target}] = Clock::now();
  const net::Endpoint mine = *me->udp;
  send(*peer_conn, {MsgType::PeerOffer, {c.user, mine.ip, std::to_string(mine.port), public_key}});
}

void Server::on_peer_accept(Connection& c, const Message& m) {
  const std::string& requester = m.field(0);
  const std::string& public_key = m.field(1);
  // Only an open offer can be answered; this stops unsolicited PeerInfo.
  if (peer_offers_.erase({requester, c.user}) == 0 || !valid_public_key(public_key)) return;
  Session* me = sessions_.find_user(c.user);
  Connection* requester_conn = connection_of(requester);
  if (!requester_conn || !me || !me->udp) return;
  const net::Endpoint mine = *me->udp;
  send(*requester_conn,
       {MsgType::PeerInfo, {c.user, mine.ip, std::to_string(mine.port), public_key}});
  BUZZ_LOG(Debug, "introduced " << requester << " and " << c.user);
}

void Server::on_peer_decline(Connection& c, const Message& m) {
  const std::string& requester = m.field(0);
  if (peer_offers_.erase({requester, c.user}) == 0) return;
  if (Connection* requester_conn = connection_of(requester)) {
    send(*requester_conn,
         {MsgType::Error, {"peer", c.user + " takes direct connections only from contacts", c.user}});
  }
}

void Server::prune_peer_offers() {
  auto deadline = Clock::now() - kOfferTimeout;
  for (auto it = peer_offers_.begin(); it != peer_offers_.end();) {
    it = it->second < deadline ? peer_offers_.erase(it) : std::next(it);
  }
}

// Fallback for direct messages when the peers cannot reach each other over
// UDP (for example behind symmetric NATs). Travels over TLS on both legs.
void Server::on_relay(Connection& c, const Message& m) {
  const std::string& target = m.field(0);
  const std::string& text = m.field(1);
  auto fail = [&](const std::string& reason) {
    send(c, {MsgType::Error, {to_string(m.type), reason, target}});
  };
  if (!valid_text(text)) return fail("text must be 1-4096 printable bytes");
  if (target == c.user) return fail("cannot message yourself");
  Connection* peer = connection_of(target);
  if (!peer) return fail(target + " is not online");
  send(*peer, {MsgType::Relayed, {c.user, text}});
  send(c, ok(MsgType::Relay, target));
}

void Server::read_udp() {
  char buffer[2048];
  for (;;) {
    sockaddr_storage address{};
    socklen_t length = sizeof address;
    ssize_t n = ::recvfrom(udp_fd_.get(), buffer, sizeof buffer, 0,
                           reinterpret_cast<sockaddr*>(&address), &length);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    try {
      Message m = decode_body(std::string_view(buffer, static_cast<size_t>(n)));
      if (m.type == MsgType::UdpHello) on_udp_hello(m, net::endpoint_from(address));
    } catch (const ProtocolError&) {
      // Ignore stray datagrams.
    }
  }
}

// Hello: user | counter | hex(HMAC(SHA-256(token), user|nonce|counter)).
// The nonce changes with every login or resume, and the counter must grow,
// so a captured hello cannot be replayed. No clocks are involved.
void Server::on_udp_hello(const Message& m, const net::Endpoint& from) {
  Session* session = sessions_.find_user(m.field(0));
  auto counter = parse_u64(m.field(1));
  if (!session || session->conn_id == 0 || !counter) return;
  if (*counter <= session->last_udp_counter) return;
  std::string expected =
      p2p::hello_mac(session->token_hash, session->user, session->udp_nonce, m.field(1));
  if (!crypto::constant_time_equal(expected, m.field(2))) return;

  session->last_udp_counter = *counter;
  session->udp = from;
  sockaddr_storage address{};
  socklen_t length = 0;
  if (!net::to_sockaddr(from, address, length)) return;
  std::string ack = encode_body({MsgType::UdpAck, {session->user}});
  ::sendto(udp_fd_.get(), ack.data(), ack.size(), 0, reinterpret_cast<sockaddr*>(&address), length);
}

}  // namespace buzz
