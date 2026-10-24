#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "buzz/user_store.hpp"

namespace buzz {
namespace {

using Result = UserStore::RegisterResult;

TEST(UserStore, RegistersAndAuthenticates) {
  UserStore store(1000);
  EXPECT_EQ(store.add_user("alice", "password123"), Result::Ok);
  EXPECT_TRUE(store.authenticate("alice", "password123"));
  EXPECT_FALSE(store.authenticate("alice", "password124"));
  EXPECT_FALSE(store.authenticate("bob", "password123"));
}

TEST(UserStore, RejectsDuplicatesAndInvalidInput) {
  UserStore store(1000);
  EXPECT_EQ(store.add_user("alice", "password123"), Result::Ok);
  EXPECT_EQ(store.add_user("alice", "different99"), Result::Exists);
  EXPECT_EQ(store.add_user("a!", "password123"), Result::Invalid);
  EXPECT_EQ(store.add_user("carol", "short"), Result::Invalid);
  EXPECT_EQ(store.size(), 1u);
}

TEST(UserStore, ConcurrentRegistrationOfSameNameHasOneWinner) {
  UserStore store(1000);
  std::atomic<int> winners{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      if (store.add_user("contested", "password123") == Result::Ok) ++winners;
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(winners.load(), 1);
  EXPECT_EQ(store.size(), 1u);
}

TEST(UserStore, ConcurrentDistinctUsers) {
  UserStore store(1000);
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&, i] {
      std::string name = "user" + std::to_string(i);
      ASSERT_EQ(store.add_user(name, "password123"), Result::Ok);
      ASSERT_TRUE(store.authenticate(name, "password123"));
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(store.size(), 8u);
}

}  // namespace
}  // namespace buzz
