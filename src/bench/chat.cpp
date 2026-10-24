// Chat load: rooms of `members` clients, `senders` of which post messages.
// Each message carries its send time, so every receiver records end-to-end
// latency through the server, including the fan-out.
#include <sys/epoll.h>

#include <atomic>
#include <memory>

#include "bench_client.hpp"
#include "buzz/net.hpp"
#include "buzz/validate.hpp"
#include "scenarios.hpp"
#include "stats.hpp"

namespace buzz::bench {
namespace {

struct Member {
  std::unique_ptr<BenchClient> client;
  std::string room;
  bool sender = false;
  int64_t next_send_ns = 0;
  uint32_t interest = EPOLLIN;
};

struct ThreadResult {
  uint64_t sent = 0;
  uint64_t delivered = 0;
  Samples latency;
};

std::string stamped_text(int payload) {
  std::string text = "t" + std::to_string(now_ns()) + " ";
  if (static_cast<int>(text.size()) < payload) {
    text.append(static_cast<size_t>(payload) - text.size(), 'x');
  }
  return text;
}

int64_t stamp_of(const std::string& text) {
  if (text.size() < 2 || text[0] != 't') return 0;
  return parse_i64(text.substr(1, text.find(' ') - 1)).value_or(0);
}

class ChatWorker {
 public:
  ChatWorker(const Options& o, std::vector<Member>& members) : o_(o), members_(members) {}

  ThreadResult run(int64_t stop_ns, int64_t drain_ns) {
    net::UniqueFd epoll(::epoll_create1(EPOLL_CLOEXEC));
    for (size_t i = 0; i < members_.size(); ++i) {
      members_[i].client->make_nonblocking();
      epoll_event event{};
      event.events = EPOLLIN;
      event.data.u64 = i;
      ::epoll_ctl(epoll.get(), EPOLL_CTL_ADD, members_[i].client->fd(), &event);
    }
    for (auto& m : members_) {
      if (!m.sender) continue;
      m.next_send_ns = now_ns();
      if (o_.rate == 0) {
        for (int w = 0; w < o_.window; ++w) post(m);
      }
    }
    std::vector<epoll_event> events(256);
    std::vector<Message> inbox;
    for (;;) {
      int64_t now = now_ns();
      if (now >= drain_ns) break;
      sending_ = now < stop_ns;
      if (sending_ && o_.rate > 0) pace(now);
      for (size_t i = 0; i < members_.size(); ++i) update_interest(epoll.get(), i);
      int n = ::epoll_wait(epoll.get(), events.data(), static_cast<int>(events.size()), 1);
      for (int e = 0; e < n; ++e) {
        Member& m = members_[events[e].data.u64];
        inbox.clear();
        if (!m.client->read_available(inbox)) throw std::runtime_error("server closed connection");
        for (const auto& message : inbox) on_message(m, message, now_ns());
      }
    }
    return std::move(result_);
  }

 private:
  void post(Member& m) {
    m.client->queue({MsgType::Say, {m.room, stamped_text(o_.payload)}});
    ++result_.sent;
  }

  // Open loop: each sender posts at a fixed rate whatever the server does.
  void pace(int64_t now) {
    const auto interval = static_cast<int64_t>(1e9 / o_.rate);
    for (auto& m : members_) {
      while (m.sender && m.next_send_ns <= now) {
        post(m);
        m.next_send_ns += interval;
      }
    }
  }

  void on_message(Member& m, const Message& message, int64_t now) {
    if (message.type != MsgType::RoomMsg) return;
    ++result_.delivered;
    int64_t stamp = stamp_of(message.field(4));
    if (stamp > 0) result_.latency.add(static_cast<double>(now - stamp) / 1000.0);
    // Closed loop: a sender's own echo frees a slot in its window.
    if (sending_ && o_.rate == 0 && m.sender && message.field(3) == m.client->name()) post(m);
  }

  void update_interest(int epoll_fd, size_t index) {
    Member& m = members_[index];
    uint32_t wanted = m.client->flush() ? (EPOLLIN | EPOLLOUT) : EPOLLIN;
    if (wanted == m.interest) return;
    epoll_event event{};
    event.events = wanted;
    event.data.u64 = index;
    ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, m.client->fd(), &event);
    m.interest = wanted;
  }

  const Options& o_;
  std::vector<Member>& members_;
  ThreadResult result_;
  bool sending_ = true;
};

}  // namespace

void run_chat(const Options& o, SSL_CTX* ctx) {
  const int total = o.rooms * o.members;
  std::vector<std::vector<Member>> per_thread(o.threads);
  auto setup = Clock::now();
  parallel(o.threads, [&](int t) {
    for (int i = t; i < total; i += o.threads) {
      Member m;
      m.room = "room" + o.prefix + "-" + std::to_string(i % o.rooms);
      m.sender = i / o.rooms < o.senders;
      m.client = std::make_unique<BenchClient>(ctx, o.port);
      m.client->sign_up(user_name(o, i), o.password);
      m.client->join(m.room);
      per_thread[t].push_back(std::move(m));
    }
  });
  result("chat_clients", total, "");
  result("chat_setup_s", seconds_since(setup), "s");

  double cpu_before = cpu_seconds(o.server_pid);
  const int64_t start = now_ns();
  const int64_t stop = start + static_cast<int64_t>(o.duration * 1e9);
  const int64_t drain = stop + 2'000'000'000;
  std::vector<ThreadResult> results(o.threads);
  parallel(o.threads, [&](int t) { results[t] = ChatWorker(o, per_thread[t]).run(stop, drain); });
  double cpu_used = cpu_seconds(o.server_pid) - cpu_before;

  ThreadResult total_result;
  for (auto& r : results) {
    total_result.sent += r.sent;
    total_result.delivered += r.delivered;
    total_result.latency.merge(r.latency);
  }
  result("chat_messages_per_sec", total_result.sent / o.duration, "1/s");
  result("chat_deliveries_per_sec", total_result.delivered / o.duration, "1/s");
  if (o.server_pid > 0) result("server_cpu_percent", cpu_used / (o.duration + 2) * 100, "%");
  report_latency("chat_latency", total_result.latency);
}

}  // namespace buzz::bench
