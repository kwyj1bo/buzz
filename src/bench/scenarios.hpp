#pragma once

#include <openssl/ssl.h>

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace buzz::bench {

struct Options {
  uint16_t port = 7000;
  std::string ca_file;
  int clients = 1000;
  int threads = 4;
  int rooms = 10;
  int members = 10;
  int senders = 1;      // Per room.
  int window = 4;       // Messages in flight per sender (closed loop).
  double rate = 0;      // Messages per second per sender; 0 uses the window.
  double duration = 10;
  int payload = 64;     // Bytes of message text.
  int messages = 1000;  // For sync and p2p.
  int server_pid = 0;   // For memory and CPU readings from /proc.
  std::string password = "password123";
  std::string prefix;   // Makes user names unique per run.
};

void run_handshakes(const Options& o, SSL_CTX* ctx);
void run_connections(const Options& o, SSL_CTX* ctx);
void run_logins(const Options& o, SSL_CTX* ctx);
void run_chat(const Options& o, SSL_CTX* ctx);
void run_sync(const Options& o, SSL_CTX* ctx);
void run_p2p(const Options& o);

// Runs fn(thread_index) on `threads` threads, waits for all of them and
// rethrows the first exception.
inline void parallel(int threads, const std::function<void(int)>& fn) {
  std::vector<std::thread> pool;
  std::vector<std::exception_ptr> errors(static_cast<size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      try {
        fn(t);
      } catch (...) {
        errors[static_cast<size_t>(t)] = std::current_exception();
      }
    });
  }
  for (auto& thread : pool) thread.join();
  for (auto& error : errors) {
    if (error) std::rethrow_exception(error);
  }
}

inline std::string user_name(const Options& o, int index) {
  return o.prefix + std::to_string(index);
}

}  // namespace buzz::bench
