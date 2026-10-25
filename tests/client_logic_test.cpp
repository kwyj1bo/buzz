// Client-side state machines behind bugs 5, 6, 7 and 9.
#include <gtest/gtest.h>

#include <vector>

#include "buzz/net.hpp"
#include "connect_policy.hpp"
#include "dm_queue.hpp"
#include "peers.hpp"
#include "room_state.hpp"

namespace buzz::client {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// Bug 7: a room becomes current only when the server confirms the join.
TEST(RoomState, FailedJoinKeepsCurrentRoom) {
  RoomState rooms;
  rooms.request_join("general");
  EXPECT_TRUE(rooms.on_joined("general"));
  rooms.request_join("full-room");  // The server answers with an error.
  EXPECT_EQ(rooms.current(), "general");
  EXPECT_FALSE(rooms.joined("full-room"));
}

TEST(RoomState, RejoinOnResumeDoesNotSwitchRooms) {
  RoomState rooms;
  rooms.request_join("a");
  rooms.on_joined("a");
  rooms.request_join("b");
  rooms.on_joined("b");
  EXPECT_FALSE(rooms.on_joined("a"));  // Resume rejoins every room.
  EXPECT_EQ(rooms.current(), "b");
}

// Bug 1 (client side): paged replay can arrive after newer live messages.
TEST(RoomState, AcceptsReplayAfterNewerLiveMessages) {
  RoomState rooms;
  rooms.on_joined("general");
  EXPECT_TRUE(rooms.accept("general", 12));  // Live.
  EXPECT_TRUE(rooms.accept("general", 10));  // Replayed.
  EXPECT_TRUE(rooms.accept("general", 11));
  EXPECT_FALSE(rooms.accept("general", 11));  // Duplicate.
  rooms.on_synced("general", 12);
  EXPECT_EQ(rooms.resume_points().front().second, 12u);
  EXPECT_FALSE(rooms.accept("other", 1));
}

// Bug 5: a request lost with the connection is sent again on reconnect.
TEST(DmQueue, RequestsAgainAfterDisconnect) {
  DmQueue queue;
  auto t0 = Clock::now();
  EXPECT_TRUE(queue.add("bob", "one", true, t0));
  EXPECT_FALSE(queue.add("bob", "two", true, t0));  // Already requested.
  queue.on_disconnect();
  EXPECT_EQ(queue.request_again(t0), std::vector<std::string>{"bob"});
  EXPECT_TRUE(queue.request_again(t0).empty());
  EXPECT_EQ(queue.take("bob"), (std::vector<std::string>{"one", "two"}));
}

TEST(DmQueue, OfflineMessagesWaitForRegistration) {
  DmQueue queue;
  auto t0 = Clock::now();
  EXPECT_FALSE(queue.add("bob", "hi", false, t0));
  EXPECT_TRUE(queue.take_expired(t0 + 1h).empty());  // Never requested, never expires.
  EXPECT_EQ(queue.request_again(t0), std::vector<std::string>{"bob"});
}

TEST(DmQueue, UnansweredRequestExpiresToRelay) {
  DmQueue queue(5s);
  auto t0 = Clock::now();
  queue.add("bob", "hi", true, t0);
  EXPECT_TRUE(queue.take_expired(t0 + 4s).empty());
  auto expired = queue.take_expired(t0 + 5s);
  ASSERT_EQ(expired.size(), 1u);
  EXPECT_EQ(expired[0].first, "bob");
  EXPECT_EQ(expired[0].second, std::vector<std::string>{"hi"});
  EXPECT_FALSE(queue.waiting("bob"));
}

// Bug 6: a peer that never acknowledges is dropped and its whole queue goes
// to the relay in order, so the next /dm asks for a fresh introduction.
TEST(PeerManager, UnreachablePeerIsDroppedAndRelayed) {
  net::UniqueFd udp = net::udp_bind(0);
  std::vector<std::string> relayed;
  PeerManager peers(
      udp.get(), [](const std::string&) {},
      [&](const std::string& peer, const std::string& text) { relayed.push_back(peer + ":" + text); });
  peers.set_self("alice");
  // Port 9 (discard) on loopback: nothing answers.
  peers.add_peer("bob", {"127.0.0.1", 9}, std::string(32, 'k'), std::string(32, 'p'));
  auto t0 = Clock::now();
  peers.send_text("bob", "first", t0);
  peers.send_text("bob", "second", t0);
  for (int i = 1; i <= 40 && peers.has_peer("bob"); ++i) peers.tick(t0 + i * 250ms);
  EXPECT_FALSE(peers.has_peer("bob"));
  EXPECT_EQ(relayed, (std::vector<std::string>{"bob:first", "bob:second"}));
}

// Bug 9: without a session the client stays idle instead of reconnecting
// every time the server times out its unauthenticated connection.
TEST(ConnectPolicy, ReconnectsOnlyWithSessionOrOnDemand) {
  ConnectPolicy policy;
  auto t0 = Clock::now();
  EXPECT_TRUE(policy.should_connect(false, t0));  // Startup.
  policy.on_connected();
  policy.on_disconnected(t0);
  EXPECT_FALSE(policy.should_connect(false, t0 + 1h));
  EXPECT_TRUE(policy.should_connect(true, t0 + 2s));
  policy.want_now();
  EXPECT_TRUE(policy.should_connect(false, t0));
}

TEST(ConnectPolicy, BacksOffOnFailure) {
  ConnectPolicy policy;
  auto t0 = Clock::now();
  policy.on_failed(t0);
  EXPECT_FALSE(policy.should_connect(true, t0 + 500ms));
  EXPECT_TRUE(policy.should_connect(true, t0 + 1s));
  policy.on_failed(t0);
  EXPECT_EQ(policy.backoff(), 4s);
}

}  // namespace
}  // namespace buzz::client
