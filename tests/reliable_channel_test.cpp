#include <gtest/gtest.h>

#include "buzz/reliable_channel.hpp"

namespace buzz {
namespace {

using namespace std::chrono_literals;
using Clock = ReliableSender::Clock;

TEST(ReliableSender, SendsOnceThenWaitsForTimeout) {
  ReliableSender sender(100ms, 3);
  auto t0 = Clock::now();
  uint64_t seq = sender.enqueue("hi", t0);
  auto first = sender.due(t0);
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(first[0].seq, seq);
  EXPECT_EQ(first[0].payload, "hi");
  EXPECT_TRUE(sender.due(t0 + 99ms).empty());
  EXPECT_EQ(sender.due(t0 + 100ms).size(), 1u);
}

TEST(ReliableSender, BacksOffExponentially) {
  ReliableSender sender(100ms, 5);
  auto t0 = Clock::now();
  sender.enqueue("hi", t0);
  sender.due(t0);                                 // Next at +100ms.
  ASSERT_EQ(sender.due(t0 + 100ms).size(), 1u);  // Next at +300ms.
  EXPECT_TRUE(sender.due(t0 + 299ms).empty());
  EXPECT_EQ(sender.due(t0 + 300ms).size(), 1u);
}

TEST(ReliableSender, AckStopsRetransmission) {
  ReliableSender sender(100ms, 3);
  auto t0 = Clock::now();
  uint64_t seq = sender.enqueue("hi", t0);
  sender.due(t0);
  EXPECT_TRUE(sender.ack(seq));
  EXPECT_FALSE(sender.ack(seq));
  EXPECT_EQ(sender.in_flight(), 0u);
  EXPECT_TRUE(sender.due(t0 + 10s).empty());
}

TEST(ReliableSender, GivesUpAfterMaxAttempts) {
  ReliableSender sender(10ms, 2);
  auto t0 = Clock::now();
  sender.enqueue("lost", t0);
  EXPECT_EQ(sender.due(t0).size(), 1u);
  EXPECT_EQ(sender.due(t0 + 10ms).size(), 1u);
  EXPECT_TRUE(sender.due(t0 + 1s).empty());
  auto failed = sender.take_failed();
  ASSERT_EQ(failed.size(), 1u);
  EXPECT_EQ(failed[0].payload, "lost");
  EXPECT_EQ(sender.in_flight(), 0u);
}

TEST(ReceiveWindow, AcceptsEachSeqOnce) {
  ReceiveWindow window;
  EXPECT_TRUE(window.accept(1));
  EXPECT_FALSE(window.accept(1));
  EXPECT_FALSE(window.accept(0));
}

TEST(ReceiveWindow, HandlesReordering) {
  ReceiveWindow window;
  EXPECT_TRUE(window.accept(3));
  EXPECT_TRUE(window.accept(1));
  EXPECT_EQ(window.contiguous(), 1u);
  EXPECT_TRUE(window.accept(2));
  EXPECT_EQ(window.contiguous(), 3u);
  EXPECT_FALSE(window.accept(3));
}

TEST(ReceiveWindow, StaysBoundedAcrossAPermanentGap) {
  ReceiveWindow window;
  for (uint64_t seq = 2; seq < 10'000; ++seq) EXPECT_TRUE(window.accept(seq));
  EXPECT_GT(window.contiguous(), 1u);
}

}  // namespace
}  // namespace buzz
