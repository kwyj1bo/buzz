#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "buzz/net.hpp"
#include "buzz/protocol.hpp"
#include "buzz/room_manager.hpp"
#include "buzz/session_manager.hpp"
#include "buzz/storage.hpp"
#include "buzz/thread_pool.hpp"
#include "buzz/tls.hpp"
#include "buzz/user_store.hpp"

namespace buzz {

struct ServerConfig {
  uint16_t port = 7000;
  uint16_t udp_port = 7000;
  std::string cert_file = "certs/server.crt";
  std::string key_file = "certs/server.key";
  std::string db_path;  // SQLite file. Empty keeps everything in memory.
  size_t worker_threads = 4;
  uint32_t pbkdf2_iterations = crypto::kDefaultPbkdf2Iterations;
  std::chrono::seconds session_ttl{300};
  size_t history_limit = 256;  // Messages per room kept in memory.
  size_t max_replay = 1000;    // Messages replayed on a rejoin, read from disk if needed.
  size_t max_rooms = 1024;
  size_t max_connections = 10'000;
  size_t max_output_buffer = 1 << 20;
};

// TLS chat server. One thread runs a level-triggered epoll loop that owns all
// connection, session and room state, so that state needs no locks. Password
// hashing runs on a worker pool, which posts results back through an eventfd.
class Server {
 public:
  explicit Server(ServerConfig config);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Loads the certificate and binds the sockets.
  void start();
  // Runs the event loop until stop() is called.
  void run();
  // Thread-safe and async-signal-safe.
  void stop() noexcept;

  uint16_t port() const { return port_; }
  uint16_t udp_port() const { return udp_port_; }

 private:
  struct Connection;
  struct AuthResult {
    uint64_t conn_id;
    MsgType kind;
    std::string user;
    bool ok;
    std::string error;
  };

  // Event loop and I/O (server.cpp).
  void dispatch(uint64_t id, uint32_t events);
  void accept_all();
  void on_connection_event(Connection& c, uint32_t events);
  bool handshake(Connection& c);
  void read_from(Connection& c);
  void send(Connection& c, const Message& message);
  void send_frame(Connection& c, const std::string& frame);
  void flush(Connection& c);
  void update_interest(Connection& c);
  void mark_close(Connection& c);
  void close_pending();
  void close_connection(uint64_t id);
  void shutdown_all();
  void watch(int fd, uint32_t events, uint64_t id);
  void post_result(AuthResult result);
  void drain_fd(int fd);
  Connection* find_connection(uint64_t id);
  Connection* connection_of(const std::string& user);

  // Protocol handlers (handlers.cpp).
  void handle(Connection& c, const Message& m);
  void on_register(Connection& c, const Message& m);
  void on_login(Connection& c, const Message& m);
  void on_resume(Connection& c, const Message& m);
  void on_logout(Connection& c);
  void on_join(Connection& c, const Message& m);
  void on_leave(Connection& c, const Message& m);
  void on_say(Connection& c, const Message& m);
  void on_who(Connection& c, const Message& m);
  void on_list_rooms(Connection& c);
  void on_peer_request(Connection& c, const Message& m);
  void process_auth_results();
  void start_session(Connection& c, const std::string& user);
  void take_over_session(const Session& old, uint64_t new_conn_id, const char* reason);
  void end_session(Connection& c);
  void detach_session(Connection& c);
  void leave_all_rooms(const std::string& user);
  void broadcast(const std::string& room, const Message& m, const std::string& skip_user = "");
  void read_udp();
  void on_udp_hello(const Message& m, const net::Endpoint& from);
  void expire_sessions();
  void restore_rooms();
  void flush_storage();
  std::vector<StoredMessage> history_for(const std::string& room, std::optional<uint64_t> since);

  ServerConfig config_;
  std::unique_ptr<Storage> storage_;
  tls::ContextPtr ctx_;
  UserStore users_;
  SessionManager sessions_;
  RoomManager rooms_;
  net::UniqueFd epoll_fd_;
  net::UniqueFd listen_fd_;
  net::UniqueFd udp_fd_;
  net::UniqueFd wake_fd_;
  net::UniqueFd timer_fd_;
  std::unordered_map<uint64_t, std::unique_ptr<Connection>> connections_;
  std::vector<uint64_t> pending_close_;
  uint64_t next_conn_id_;
  std::mutex results_mutex_;
  std::vector<AuthResult> results_;
  std::atomic<bool> stopping_{false};
  uint16_t port_ = 0;
  uint16_t udp_port_ = 0;
  // Declared last so it is destroyed first: its tasks use users_ and results_.
  ThreadPool pool_;
};

}  // namespace buzz
