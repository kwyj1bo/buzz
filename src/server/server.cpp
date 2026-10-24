#include "buzz/server.hpp"

#include <openssl/err.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <system_error>

#include "buzz/log.hpp"
#include "connection.hpp"

namespace buzz {
namespace {

constexpr uint64_t kListenerId = 1;
constexpr uint64_t kUdpId = 2;
constexpr uint64_t kWakeId = 3;
constexpr uint64_t kTimerId = 4;
constexpr uint64_t kFirstConnectionId = 16;
constexpr size_t kReadChunk = 16 * 1024;
constexpr int kMaxReadsPerEvent = 16;

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

net::UniqueFd make_timer() {
  net::UniqueFd fd(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
  if (!fd.valid()) throw_errno("timerfd_create");
  itimerspec spec{};
  spec.it_interval.tv_sec = 1;
  spec.it_value.tv_sec = 1;
  if (::timerfd_settime(fd.get(), 0, &spec, nullptr) != 0) throw_errno("timerfd_settime");
  return fd;
}

bool would_block(int ssl_error) {
  return ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE;
}

}  // namespace

Server::Server(ServerConfig config)
    : config_(std::move(config)),
      storage_(config_.db_path.empty() ? nullptr : std::make_unique<Storage>(config_.db_path)),
      users_(config_.pbkdf2_iterations, storage_.get()),
      sessions_(config_.session_ttl),
      rooms_(config_.history_limit, config_.max_rooms),
      wake_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      next_conn_id_(kFirstConnectionId),
      pool_(config_.worker_threads) {
  if (!wake_fd_.valid()) throw_errno("eventfd");
}

Server::~Server() = default;

void Server::start() {
  net::ignore_sigpipe();
  ctx_ = tls::make_server_context(config_.cert_file, config_.key_file);
  listen_fd_ = net::tcp_listen(config_.port);
  port_ = net::local_port(listen_fd_.get());
  udp_fd_ = net::udp_bind(config_.udp_port);
  udp_port_ = net::local_port(udp_fd_.get());
  timer_fd_ = make_timer();
  epoll_fd_.reset(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd_.valid()) throw_errno("epoll_create1");

  watch(listen_fd_.get(), EPOLLIN, kListenerId);
  watch(udp_fd_.get(), EPOLLIN, kUdpId);
  watch(wake_fd_.get(), EPOLLIN, kWakeId);
  watch(timer_fd_.get(), EPOLLIN, kTimerId);
  restore_rooms();
  BUZZ_LOG(Info, "listening on tcp " << port_ << " (TLS) and udp " << udp_port_ << " with "
                                     << pool_.size() << " auth workers");
}

void Server::run() {
  std::array<epoll_event, 256> events{};
  while (!stopping_.load()) {
    int count = ::epoll_wait(epoll_fd_.get(), events.data(), static_cast<int>(events.size()), -1);
    if (count < 0) {
      if (errno == EINTR) continue;
      throw_errno("epoll_wait");
    }
    for (int i = 0; i < count; ++i) dispatch(events[i].data.u64, events[i].events);
    close_pending();
    flush_storage();
  }
  shutdown_all();
  flush_storage();
  BUZZ_LOG(Info, "server stopped");
}

void Server::stop() noexcept {
  stopping_.store(true);
  uint64_t one = 1;
  if (wake_fd_.valid()) {
    [[maybe_unused]] ssize_t n = ::write(wake_fd_.get(), &one, sizeof one);
  }
}

void Server::dispatch(uint64_t id, uint32_t events) {
  switch (id) {
    case kListenerId: return accept_all();
    case kUdpId: return read_udp();
    case kWakeId:
      drain_fd(wake_fd_.get());
      return process_auth_results();
    case kTimerId:
      drain_fd(timer_fd_.get());
      return expire_sessions();
    default:
      if (Connection* c = find_connection(id); c && !c->closing) on_connection_event(*c, events);
  }
}

void Server::accept_all() {
  for (;;) {
    sockaddr_storage address{};
    socklen_t length = sizeof address;
    int fd = ::accept4(listen_fd_.get(), reinterpret_cast<sockaddr*>(&address), &length,
                       SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) BUZZ_LOG(Warn, "accept failed: errno " << errno);
      return;
    }
    net::UniqueFd owned(fd);
    if (connections_.size() >= config_.max_connections) {
      BUZZ_LOG(Warn, "connection limit reached, dropping " << net::endpoint_from(address).to_string());
      continue;
    }
    net::set_no_delay(fd);
    auto c = std::make_unique<Connection>();
    c->id = next_conn_id_++;
    c->peer = net::endpoint_from(address).to_string();
    c->ssl.reset(SSL_new(ctx_.get()));
    if (!c->ssl || SSL_set_fd(c->ssl.get(), fd) != 1) {
      BUZZ_LOG(Error, "SSL_new failed: " << tls::last_error());
      continue;
    }
    SSL_set_accept_state(c->ssl.get());
    c->fd = std::move(owned);
    c->interest = EPOLLIN | EPOLLRDHUP;
    watch(fd, c->interest, c->id);
    BUZZ_LOG(Debug, "accepted " << c->peer << " as #" << c->id);
    connections_.emplace(c->id, std::move(c));
  }
}

void Server::on_connection_event(Connection& c, uint32_t events) {
  if (events & EPOLLERR) return mark_close(c);
  if (!c.handshake_done && !handshake(c)) return update_interest(c);
  read_from(c);
  if (!c.closing) flush(c);
}

bool Server::handshake(Connection& c) {
  ERR_clear_error();
  int rc = SSL_accept(c.ssl.get());
  if (rc == 1) {
    c.handshake_done = true;
    c.handshake_wants_write = false;
    BUZZ_LOG(Debug, "#" << c.id << " TLS " << SSL_get_version(c.ssl.get()));
    return true;
  }
  int error = SSL_get_error(c.ssl.get(), rc);
  if (would_block(error)) {
    c.handshake_wants_write = error == SSL_ERROR_WANT_WRITE;
    return false;
  }
  BUZZ_LOG(Debug, "#" << c.id << " handshake failed: " << tls::last_error());
  mark_close(c);
  return false;
}

void Server::read_from(Connection& c) {
  char buffer[kReadChunk];
  for (int i = 0; i < kMaxReadsPerEvent && !c.closing; ++i) {
    ERR_clear_error();
    int n = SSL_read(c.ssl.get(), buffer, sizeof buffer);
    if (n <= 0) {
      if (!would_block(SSL_get_error(c.ssl.get(), n))) mark_close(c);
      return;
    }
    c.decoder.feed(buffer, static_cast<size_t>(n));
    try {
      while (!c.closing) {
        auto message = c.decoder.next();
        if (!message) break;
        handle(c, *message);
      }
    } catch (const ProtocolError& e) {
      BUZZ_LOG(Debug, "#" << c.id << " protocol error: " << e.what());
      send(c, {MsgType::Error, {"protocol", e.what()}});
      mark_close(c);
    }
  }
}

void Server::send(Connection& c, const Message& message) { send_frame(c, encode_frame(message)); }

void Server::send_frame(Connection& c, const std::string& frame) {
  if (c.closing || !c.handshake_done) return;
  c.out += frame;
  if (c.out.size() > config_.max_output_buffer) {
    BUZZ_LOG(Warn, "#" << c.id << " too slow, output buffer full");
    return mark_close(c);
  }
  flush(c);
}

void Server::flush(Connection& c) {
  while (!c.out.empty()) {
    ERR_clear_error();
    int chunk = static_cast<int>(std::min<size_t>(c.out.size(), INT_MAX));
    int n = SSL_write(c.ssl.get(), c.out.data(), chunk);
    if (n > 0) {
      c.out.erase(0, static_cast<size_t>(n));
      continue;
    }
    if (!would_block(SSL_get_error(c.ssl.get(), n))) return mark_close(c);
    break;
  }
  update_interest(c);
}

void Server::update_interest(Connection& c) {
  if (c.closing) return;
  uint32_t wanted = EPOLLIN | EPOLLRDHUP;
  if (!c.out.empty() || c.handshake_wants_write) wanted |= EPOLLOUT;
  if (wanted == c.interest) return;
  epoll_event event{};
  event.events = wanted;
  event.data.u64 = c.id;
  ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, c.fd.get(), &event);
  c.interest = wanted;
}

// Closing is deferred to the end of the loop iteration, so handlers never
// see a connection freed under them.
void Server::mark_close(Connection& c) {
  if (c.closing) return;
  c.closing = true;
  pending_close_.push_back(c.id);
}

void Server::close_pending() {
  while (!pending_close_.empty()) {
    std::vector<uint64_t> ids;
    ids.swap(pending_close_);
    for (uint64_t id : ids) close_connection(id);
  }
}

void Server::close_connection(uint64_t id) {
  auto it = connections_.find(id);
  if (it == connections_.end()) return;
  std::unique_ptr<Connection> c = std::move(it->second);
  connections_.erase(it);
  ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, c->fd.get(), nullptr);
  if (c->handshake_done) SSL_shutdown(c->ssl.get());
  detach_session(*c);
  BUZZ_LOG(Debug, "closed #" << id << " (" << c->peer << ")");
}

void Server::shutdown_all() {
  for (auto& entry : connections_) {
    if (entry.second->handshake_done) SSL_shutdown(entry.second->ssl.get());
  }
  connections_.clear();
}

void Server::watch(int fd, uint32_t events, uint64_t id) {
  epoll_event event{};
  event.events = events;
  event.data.u64 = id;
  if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &event) != 0) throw_errno("epoll_ctl");
}

void Server::post_result(AuthResult result) {
  {
    std::lock_guard lock(results_mutex_);
    results_.push_back(std::move(result));
  }
  uint64_t one = 1;
  [[maybe_unused]] auto n = ::write(wake_fd_.get(), &one, sizeof one);
}

void Server::drain_fd(int fd) {
  uint64_t value = 0;
  while (::read(fd, &value, sizeof value) > 0) {
  }
}

// Group commit: everything posted during one loop pass is written in a
// single SQLite transaction.
void Server::flush_storage() {
  if (!storage_) return;
  try {
    storage_->flush();
  } catch (const StorageError& e) {
    BUZZ_LOG(Error, "storage flush failed: " << e.what());
  }
}

void Server::restore_rooms() {
  if (!storage_) return;
  auto rooms = storage_->load_rooms(config_.history_limit);
  for (auto& room : rooms) rooms_.restore(room.room, room.last_seq, std::move(room.recent));
  BUZZ_LOG(Info, "loaded " << users_.size() << " users and " << rooms.size() << " rooms from "
                           << config_.db_path);
}

Server::Connection* Server::find_connection(uint64_t id) {
  auto it = connections_.find(id);
  return it == connections_.end() ? nullptr : it->second.get();
}

Server::Connection* Server::connection_of(const std::string& user) {
  Session* session = sessions_.find_user(user);
  if (!session || session->conn_id == 0) return nullptr;
  Connection* c = find_connection(session->conn_id);
  return c && !c->closing ? c : nullptr;
}

}  // namespace buzz
