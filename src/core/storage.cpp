#include "buzz/storage.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>

namespace buzz {
namespace {

constexpr const char* kSchema = R"(
CREATE TABLE IF NOT EXISTS users (
  name       TEXT PRIMARY KEY,
  salt       BLOB NOT NULL,
  hash       BLOB NOT NULL,
  iterations INTEGER NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS messages (
  room   TEXT NOT NULL,
  seq    INTEGER NOT NULL,
  ts     INTEGER NOT NULL,
  sender TEXT NOT NULL,
  text   TEXT NOT NULL,
  PRIMARY KEY (room, seq)
) WITHOUT ROWID;
PRAGMA user_version = 1;
)";

int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

// Owns a prepared statement.
class Storage::Statement {
 public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) fail("prepare");
  }
  ~Statement() { sqlite3_finalize(stmt_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  Statement& bind(int index, const std::string& value) {
    check(sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                            SQLITE_TRANSIENT));
    return *this;
  }
  Statement& bind_blob(int index, const std::string& value) {
    check(sqlite3_bind_blob(stmt_, index, value.data(), static_cast<int>(value.size()),
                            SQLITE_TRANSIENT));
    return *this;
  }
  Statement& bind(int index, int64_t value) {
    check(sqlite3_bind_int64(stmt_, index, value));
    return *this;
  }

  // True while a row is available.
  bool step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    fail("step");
  }
  void reset() { sqlite3_reset(stmt_); }
  // Runs to completion. Returns the SQLite result code.
  int run() {
    int rc = sqlite3_step(stmt_);
    sqlite3_reset(stmt_);
    return rc;
  }

  int64_t integer(int column) const { return sqlite3_column_int64(stmt_, column); }
  std::string text(int column) const {
    auto data = static_cast<const char*>(sqlite3_column_blob(stmt_, column));
    return data ? std::string(data, static_cast<size_t>(sqlite3_column_bytes(stmt_, column))) : "";
  }

 private:
  void check(int rc) {
    if (rc != SQLITE_OK) fail("bind");
  }
  [[noreturn]] void fail(const char* what) {
    throw StorageError(std::string("sqlite ") + what + ": " + sqlite3_errmsg(db_));
  }

  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};

Storage::Storage(const std::string& path) {
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
    std::string error = db_ ? sqlite3_errmsg(db_) : "out of memory";
    sqlite3_close(db_);
    throw StorageError("open " + path + ": " + error);
  }
  sqlite3_busy_timeout(db_, 5000);
  exec("PRAGMA journal_mode = WAL");
  // With WAL, NORMAL is crash-safe; only the last commits can roll back
  // after a power loss.
  exec("PRAGMA synchronous = NORMAL");
  exec(kSchema);
}

Storage::~Storage() {
  try {
    flush();
  } catch (const StorageError&) {
  }
  sqlite3_close(db_);
}

bool Storage::insert_user(const std::string& name, const crypto::PasswordHash& hash) {
  std::lock_guard lock(mutex_);
  Statement insert(db_,
                   "INSERT OR IGNORE INTO users (name, salt, hash, iterations, created_at) "
                   "VALUES (?, ?, ?, ?, ?)");
  insert.bind(1, name).bind_blob(2, hash.salt).bind_blob(3, hash.hash);
  insert.bind(4, static_cast<int64_t>(hash.iterations)).bind(5, now_ms());
  if (insert.run() != SQLITE_DONE) throw StorageError(sqlite3_errmsg(db_));
  return sqlite3_changes(db_) == 1;
}

std::vector<std::pair<std::string, crypto::PasswordHash>> Storage::load_users() {
  std::lock_guard lock(mutex_);
  Statement select(db_, "SELECT name, salt, hash, iterations FROM users");
  std::vector<std::pair<std::string, crypto::PasswordHash>> users;
  while (select.step()) {
    users.push_back({select.text(0),
                     {select.text(1), select.text(2), static_cast<uint32_t>(select.integer(3))}});
  }
  return users;
}

void Storage::append_message(const std::string& room, const StoredMessage& message) {
  std::lock_guard lock(mutex_);
  queue_.push_back({room, message});
}

size_t Storage::flush() {
  std::lock_guard lock(mutex_);
  return flush_locked();
}

size_t Storage::flush_locked() {
  if (queue_.empty()) return 0;
  exec("BEGIN");
  try {
    Statement insert(db_,
                     "INSERT OR REPLACE INTO messages (room, seq, ts, sender, text) "
                     "VALUES (?, ?, ?, ?, ?)");
    for (const auto& q : queue_) {
      insert.bind(1, q.room).bind(2, static_cast<int64_t>(q.message.seq));
      insert.bind(3, q.message.timestamp_ms).bind(4, q.message.from).bind(5, q.message.text);
      if (insert.run() != SQLITE_DONE) throw StorageError(sqlite3_errmsg(db_));
    }
    exec("COMMIT");
  } catch (...) {
    exec("ROLLBACK");
    throw;
  }
  size_t written = queue_.size();
  queue_.clear();
  return written;
}

std::vector<Storage::RoomHistory> Storage::load_rooms(size_t recent_limit) {
  std::lock_guard lock(mutex_);
  flush_locked();
  std::vector<RoomHistory> rooms;
  Statement select_rooms(db_, "SELECT room, MAX(seq) FROM messages GROUP BY room");
  while (select_rooms.step()) {
    rooms.push_back({select_rooms.text(0), static_cast<uint64_t>(select_rooms.integer(1)), {}});
  }
  Statement select_recent(db_,
                          "SELECT seq, ts, sender, text FROM messages WHERE room = ? "
                          "ORDER BY seq DESC LIMIT ?");
  for (auto& room : rooms) {
    select_recent.bind(1, room.room).bind(2, static_cast<int64_t>(recent_limit));
    while (select_recent.step()) {
      room.recent.push_back({static_cast<uint64_t>(select_recent.integer(0)),
                             select_recent.integer(1), select_recent.text(2),
                             select_recent.text(3)});
    }
    select_recent.reset();
    std::reverse(room.recent.begin(), room.recent.end());
  }
  return rooms;
}

std::vector<StoredMessage> Storage::history_since(const std::string& room, uint64_t since,
                                                  size_t limit) {
  std::lock_guard lock(mutex_);
  flush_locked();
  // Newest `limit` messages after `since`, returned oldest first.
  Statement select(db_,
                   "SELECT seq, ts, sender, text FROM messages WHERE room = ? AND seq > ? "
                   "ORDER BY seq DESC LIMIT ?");
  select.bind(1, room).bind(2, static_cast<int64_t>(since)).bind(3, static_cast<int64_t>(limit));
  std::vector<StoredMessage> out;
  while (select.step()) {
    out.push_back({static_cast<uint64_t>(select.integer(0)), select.integer(1), select.text(2),
                   select.text(3)});
  }
  std::reverse(out.begin(), out.end());
  return out;
}

void Storage::exec(const char* sql) {
  char* error = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    std::string message = error ? error : "unknown error";
    sqlite3_free(error);
    throw StorageError(std::string("sqlite exec: ") + message);
  }
}

}  // namespace buzz
