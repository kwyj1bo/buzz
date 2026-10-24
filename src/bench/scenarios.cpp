// Setup-heavy scenarios: handshakes, idle connections, logins, history
// replay and the peer-to-peer channel.
#include "scenarios.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

#include <memory>

#include "bench_client.hpp"
#include "buzz/crypto.hpp"
#include "buzz/p2p.hpp"
#include "stats.hpp"

namespace buzz::bench {
namespace {

using ClientList = std::vector<std::unique_ptr<BenchClient>>;

double ping_rtt(BenchClient& client, int count, Samples& samples) {
  auto start = Clock::now();
  for (int i = 0; i < count; ++i) {
    auto sent = Clock::now();
    client.send({MsgType::Ping, {std::to_string(i)}});
    if (!client.expect(MsgType::Pong, std::chrono::seconds(10))) throw std::runtime_error("no pong");
    samples.add(seconds_since(sent) * 1e6);
  }
  return count / seconds_since(start);
}

std::string datagram_roundtrip(int fd, const sockaddr_in& to, const std::string& data) {
  ::sendto(fd, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
  char buffer[2048];
  ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
  return n > 0 ? std::string(buffer, static_cast<size_t>(n)) : "";
}

sockaddr_in loopback(uint16_t port) {
  sockaddr_storage storage{};
  socklen_t length = 0;
  net::to_sockaddr({"127.0.0.1", port}, storage, length);
  return reinterpret_cast<sockaddr_in&>(storage);
}

}  // namespace

void run_handshakes(const Options& o, SSL_CTX* ctx) {
  std::vector<Samples> samples(o.threads);
  std::vector<ClientList> held(o.threads);
  auto start = Clock::now();
  parallel(o.threads, [&](int t) {
    for (int i = t; i < o.clients; i += o.threads) {
      held[t].push_back(std::make_unique<BenchClient>(ctx, o.port));
      samples[t].add(held[t].back()->handshake_micros());
    }
  });
  double elapsed = seconds_since(start);
  Samples all;
  for (auto& s : samples) all.merge(s);
  result("handshakes_per_sec", o.clients / elapsed, "1/s");
  report_latency("handshake", all);
}

// Opens many logged-in connections, then measures server memory and the
// round trip time of one more client while they stay connected.
void run_connections(const Options& o, SSL_CTX* ctx) {
  long rss_before = rss_kib(o.server_pid);
  std::vector<ClientList> held(o.threads);
  auto start = Clock::now();
  parallel(o.threads, [&](int t) {
    for (int i = t; i < o.clients; i += o.threads) {
      held[t].push_back(std::make_unique<BenchClient>(ctx, o.port));
      held[t].back()->sign_up(user_name(o, i), o.password);
    }
  });
  result("connections", o.clients, "");
  result("connect_login_per_sec", o.clients / seconds_since(start), "1/s");

  long rss_after = rss_kib(o.server_pid);
  if (rss_after > 0) {
    result("server_rss_mib", rss_after / 1024.0, "MiB");
    result("rss_per_connection_kib", static_cast<double>(rss_after - rss_before) / o.clients, "KiB");
  }
  BenchClient probe(ctx, o.port);
  probe.sign_up(user_name(o, o.clients), o.password);
  Samples rtt;
  ping_rtt(probe, 2000, rtt);
  report_latency("ping_rtt_loaded", rtt);
}

void run_logins(const Options& o, SSL_CTX* ctx) {
  // One connection per thread keeps the server's worker pool busy.
  std::vector<std::unique_ptr<BenchClient>> clients;
  for (int t = 0; t < o.threads; ++t) clients.push_back(std::make_unique<BenchClient>(ctx, o.port));

  auto start = Clock::now();
  parallel(o.threads, [&](int t) {
    for (int i = t; i < o.clients; i += o.threads) {
      clients[t]->send({MsgType::Register, {user_name(o, i), o.password}});
      if (!clients[t]->expect(MsgType::Ok, std::chrono::seconds(120))) throw std::runtime_error("register");
    }
  });
  result("registrations_per_sec", o.clients / seconds_since(start), "1/s");

  std::vector<Samples> samples(o.threads);
  start = Clock::now();
  parallel(o.threads, [&](int t) {
    for (int i = t; i < o.clients; i += o.threads) {
      auto sent = Clock::now();
      clients[t]->login(user_name(o, i), o.password);
      samples[t].add(seconds_since(sent) * 1e6);
    }
  });
  result("logins_per_sec", o.clients / seconds_since(start), "1/s");
  Samples all;
  for (auto& s : samples) all.merge(s);
  report_latency("login", all);
}

// Posts `messages` to a room, then times how long a new member takes to
// replay all of them with Join(since = 0).
void run_sync(const Options& o, SSL_CTX* ctx) {
  const std::string room = "sync" + o.prefix;
  BenchClient writer(ctx, o.port);
  writer.sign_up(user_name(o, 0), o.password);
  writer.join(room);
  for (int i = 0; i < o.messages; ++i) {
    writer.send({MsgType::Say, {room, "history message " + std::to_string(i)}});
  }
  for (int i = 0; i < o.messages; ++i) {
    if (!writer.expect(MsgType::RoomMsg, std::chrono::seconds(30))) throw std::runtime_error("post");
  }

  BenchClient reader(ctx, o.port);
  reader.sign_up(user_name(o, 1), o.password);
  auto start = Clock::now();
  reader.send({MsgType::Join, {room, "0"}});
  if (!reader.expect(MsgType::SyncDone, std::chrono::seconds(30))) throw std::runtime_error("sync");
  double elapsed = seconds_since(start);
  result("replay_messages", o.messages, "");
  result("replay_ms", elapsed * 1000, "ms");
  result("replay_msgs_per_sec", o.messages / elapsed, "1/s");
}

// Encrypted datagram ping-pong between two local peers, as the client does.
void run_p2p(const Options& o) {
  const std::string key = crypto::random_bytes(crypto::kKeySize);
  net::UniqueFd a = net::udp_bind(0);
  net::UniqueFd b = net::udp_bind(0);
  net::set_nonblocking(a.get(), false);
  net::set_nonblocking(b.get(), false);
  sockaddr_in to_a = loopback(net::local_port(a.get()));
  sockaddr_in to_b = loopback(net::local_port(b.get()));

  std::thread echo([&] {
    char buffer[2048];
    for (int i = 0; i < o.messages; ++i) {
      ssize_t n = ::recv(b.get(), buffer, sizeof buffer, 0);
      auto message = p2p::open_datagram(std::string_view(buffer, static_cast<size_t>(n)), key);
      std::string ack = p2p::seal_datagram("bob", key, {MsgType::DmAck, {"bob", message->field(1)}});
      ::sendto(b.get(), ack.data(), ack.size(), 0, reinterpret_cast<sockaddr*>(&to_a), sizeof to_a);
    }
  });
  Samples rtt;
  std::string text(static_cast<size_t>(o.payload), 'x');
  auto start = Clock::now();
  for (int i = 0; i < o.messages; ++i) {
    auto sent = Clock::now();
    std::string data =
        p2p::seal_datagram("alice", key, {MsgType::DmData, {"alice", std::to_string(i), text}});
    auto reply = datagram_roundtrip(a.get(), to_b, data);
    if (!p2p::open_datagram(reply, key)) throw std::runtime_error("bad ack");
    rtt.add(seconds_since(sent) * 1e6);
  }
  double elapsed = seconds_since(start);
  echo.join();
  result("p2p_roundtrips_per_sec", o.messages / elapsed, "1/s");
  report_latency("p2p_rtt", rtt);
}

}  // namespace buzz::bench
