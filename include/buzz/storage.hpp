#pragma once

#include <cstdint>
#include <mutex>
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

// SQLite store for accounts and room history.
// The database runs in WAL mode. Messages are queued by append_message and
// written by flush in one transaction, so the server commits once per event
// loop pass instead of once per message. All methods are thread-safe.
class Storage {
 public:
  struct RoomHistory {
    std::string room;
    uint64_t last_seq = 0;
    std::vector<StoredMessage> recent;  // Oldest first.
  };

  // Opens or creates the database. Throws StorageError.
  explicit Storage(const std::string& path);
  ~Storage();
  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  // Returns false when the user already exists.
  bool insert_user(const std::string& name, const crypto::PasswordHash& hash);
  std::vector<std::pair<std::string, crypto::PasswordHash>> load_users();

  void append_message(const std::string& room, const StoredMessage& message);
  // Commits queued messages. Returns how many were written.
  size_t flush();
  std::vector<RoomHistory> load_rooms(size_t recent_limit);
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

  std::mutex mutex_;
  sqlite3* db_ = nullptr;
  std::vector<Queued> queue_;
};

}  // namespace buzz
