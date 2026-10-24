#include <gtest/gtest.h>

#include <atomic>

#include "buzz/thread_pool.hpp"

namespace buzz {
namespace {

TEST(ThreadPool, RunsEveryTaskBeforeDestruction) {
  std::atomic<int> done{0};
  {
    ThreadPool pool(4);
    for (int i = 0; i < 1000; ++i) pool.submit([&] { ++done; });
  }
  EXPECT_EQ(done.load(), 1000);
}

TEST(ThreadPool, SurvivesThrowingTask) {
  std::atomic<int> done{0};
  {
    ThreadPool pool(1);
    pool.submit([] { throw std::runtime_error("boom"); });
    pool.submit([&] { ++done; });
  }
  EXPECT_EQ(done.load(), 1);
}

TEST(ThreadPool, ZeroThreadsMeansOne) { EXPECT_EQ(ThreadPool(0).size(), 1u); }

}  // namespace
}  // namespace buzz
