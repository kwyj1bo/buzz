#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "buzz/log.hpp"

namespace buzz {

// Fixed-size worker pool. The server uses it for password hashing, so slow
// PBKDF2 work never blocks the event loop. The destructor runs all queued
// tasks before it joins the workers.
class ThreadPool {
 public:
  explicit ThreadPool(size_t threads) {
    if (threads == 0) threads = 1;
    workers_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { work(); });
  }

  ~ThreadPool() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_all();
    for (auto& worker : workers_) worker.join();
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  void submit(std::function<void()> task) {
    {
      std::lock_guard lock(mutex_);
      tasks_.push(std::move(task));
    }
    ready_.notify_one();
  }

  size_t size() const { return workers_.size(); }

 private:
  void work() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty()) return;
        task = std::move(tasks_.front());
        tasks_.pop();
      }
      try {
        task();
      } catch (const std::exception& e) {
        BUZZ_LOG(Error, "worker task failed: " << e.what());
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable ready_;
  std::queue<std::function<void()>> tasks_;
  bool stopping_ = false;
  std::vector<std::thread> workers_;
};

}  // namespace buzz
