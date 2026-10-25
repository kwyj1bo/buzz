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
CREATE TABLE IF NOT EXISTS sessions (
  user       TEXT PRIMARY KEY,
  token_hash BLOB NOT NULL,
  created_at INTEGER NOT NULL
);
PRAGMA user_version = 2;
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

Storage::Storage(const std::string& path, size_t max_queued) : max_queued_(max_queued) {
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

void Storage::save_session(const std::string& user, const std::string& token_hash) {
  std::lock_guard lock(mutex_);
  Statement upsert(db_,
                   "INSERT OR REPLACE INTO sessions (user, token_hash, created_at) "
                   "VALUES (?, ?, ?)");
  upsert.bind(1, user).bind_blob(2, token_hash).bind(3, now_ms());
  if (upsert.run() != SQLITE_DONE) throw StorageError(sqlite3_errmsg(db_));
}

void Storage::delete_session(const std::string& user) {
  std::lock_guard lock(mutex_);
  Statement remove(db_, "DELETE FROM sessions WHERE user = ?");
  remove.bind(1, user);
  if (remove.run() != SQLITE_DONE) throw StorageError(sqlite3_errmsg(db_));
}

std::vector<std::pair<std::string, std::string>> Storage::load_sessions() {
  std::lock_guard lock(mutex_);
  Statement select(db_, "SELECT user, token_hash FROM sessions");
  std::vector<std::pair<std::string, std::string>> sessions;
  while (select.step()) sessions.emplace_back(select.text(0), select.text(1));
  return sessions;
}

void Storage::append_message(const std::string& room, const StoredMessage& message) {
  std::lock_guard lock(mutex_);
  if (queue_.size() >= max_queued_) {
    queue_.pop_front();
    ++dropped_;
  }
  queue_.push_back({room, message});
}

size_t Storage::pending() {
  std::lock_guard lock(mutex_);
  return queue_.size();
}

uint64_t Storage::dropped() {
  std::lock_guard lock(mutex_);
  return dropped_;
}

size_t Storage::flush() {
  std::lock_guard lock(mutex_);
  return flush_locked();
}

// Reads should see queued messages, but a failing write must not fail them.
void Storage::flush_for_read() {
  try {
    flush_locked();
  } catch (const StorageError&) {
  }
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

std::optional<Storage::RoomHistory> Storage::load_room(const std::string& room,
                                                      size_t recent_limit) {
  std::lock_guard lock(mutex_);
  flush_for_read();
  Statement select(db_,
                   "SELECT seq, ts, sender, text FROM messages WHERE room = ? "
                   "ORDER BY seq DESC LIMIT ?");
  select.bind(1, room).bind(2, static_cast<int64_t>(std::max<size_t>(recent_limit, 1)));
  RoomHistory history{room, 0, {}};
  while (select.step()) {
    history.recent.push_back({static_cast<uint64_t>(select.integer(0)), select.integer(1),
                              select.text(2), select.text(3)});
  }
  if (history.recent.empty()) return std::nullopt;
  history.last_seq = history.recent.front().seq;
  if (recent_limit == 0) history.recent.clear();
  std::reverse(history.recent.begin(), history.recent.end());
  return history;
}

std::vector<StoredMessage> Storage::history_since(const std::string& room, uint64_t since,
                                                  size_t limit) {
  std::lock_guard lock(mutex_);
  flush_for_read();
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
