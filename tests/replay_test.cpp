// Bug 1: a large history replay used to overflow the 1 MiB output buffer
// and disconnect the client that was trying to catch up.
#include <gtest/gtest.h>

#include "support/test_client.hpp"
#include "support/test_server.hpp"

namespace buzz::test {
namespace {

TEST(Replay, LargeBacklogIsPagedNotDropped) {
  constexpr int kMessages = 600;  // About 2.4 MB of history.
  TestServer server("", [](ServerConfig& c) {
    c.history_limit = 1000;
    c.max_replay = 1000;
  });
  const std::string text(4000, 'x');
  {
    TestClient writer(server.cert_file(), server.port());
    writer.sign_up("writer");
    writer.send({MsgType::Join, {"busy"}});
    ASSERT_TRUE(writer.expect(MsgType::SyncDone).has_value());
    for (int i = 0; i < kMessages; ++i) {
      writer.send({MsgType::Say, {"busy", text}});
      ASSERT_TRUE(writer.expect(MsgType::RoomMsg).has_value());
    }
  }

  TestClient reader(server.cert_file(), server.port());
  reader.sign_up("reader");
  reader.send({MsgType::Join, {"busy", "0"}});
  int replayed = 0;
  for (;;) {
    auto m = reader.receive(10s);
    ASSERT_TRUE(m.has_value()) << "disconnected after " << replayed << " messages";
    if (m->type == MsgType::RoomMsg) ++replayed;
    if (m->type == MsgType::SyncDone) {
      EXPECT_EQ(m->field(1), std::to_string(kMessages));
      break;
    }
  }
  EXPECT_EQ(replayed, kMessages);
  reader.send({MsgType::Ping, {"still connected"}});
  EXPECT_TRUE(reader.expect(MsgType::Pong).has_value());
}

}  // namespace
}  // namespace buzz::test
