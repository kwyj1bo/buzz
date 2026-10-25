#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "buzz/crypto.hpp"
#include "buzz/room_manager.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace buzz {

class StorageError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// SQLite store for accounts, sessions and room history.
// The database runs in WAL mode. Messages are queued by append_message and
// written by flush in one transaction, so the server commits once per event
// loop pass instead of once per message. If writes keep failing (for example
// on a full disk), the queue keeps only the newest `max_queued` messages.
// All methods are thread-safe.
class Storage {
 public:
  struct RoomHistory {
    std::string room;
    uint64_t last_seq = 0;
    std::vector<StoredMessage> recent;  // Oldest first.
  };

  // Opens or creates the database. Throws StorageError.
  explicit Storage(const std::string& path, size_t max_queued = 100'000);
  ~Storage();
  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  // Returns false when the user already exists.
  bool insert_user(const std::string& name, const crypto::PasswordHash& hash);
  std::vector<std::pair<std::string, crypto::PasswordHash>> load_users();

  // Sessions store only a hash of the token.
  void save_session(const std::string& user, const std::string& token_hash);
  void delete_session(const std::string& user);
  std::vector<std::pair<std::string, std::string>> load_sessions();

  void append_message(const std::string& room, const StoredMessage& message);
  // Commits queued messages. Returns how many were written. On failure the
  // messages stay queued and StorageError is thrown.
  size_t flush();
  size_t pending();
  // Messages dropped because the queue was full.
  uint64_t dropped();
  // Last seq and newest messages of a room, or nullopt if it has none.
  std::optional<RoomHistory> load_room(const std::string& room, size_t recent_limit);
  // Messages with seq > since, at most `limit`, oldest first. Flushes first.
  std::vector<StoredMessage> history_since(const std::string& room, uint64_t since, size_t limit);

 private:
  class Statement;
  struct Queued {
    std::string room;
    StoredMessage message;
  };

  void exec(const char* sql);
  size_t flush_locked();
  void flush_for_read();

  std::mutex mutex_;
  sqlite3* db_ = nullptr;
  size_t max_queued_;
  std::deque<Queued> queue_;
  uint64_t dropped_ = 0;
};

}  // namespace buzz
