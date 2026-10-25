add_library(buzz_core
  core/auth_throttle.cpp
  core/crypto.cpp
  core/log.cpp
  core/net.cpp
  core/p2p.cpp
  core/protocol.cpp
  core/reliable_channel.cpp
  core/room_manager.cpp
  core/session_manager.cpp
  core/storage.cpp
  core/tls.cpp
  core/user_store.cpp
  core/validate.cpp
)
target_include_directories(buzz_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(buzz_core PUBLIC OpenSSL::SSL OpenSSL::Crypto SQLite::SQLite3 Threads::Threads)

add_library(buzz_server_lib
  server/server.cpp
  server/handlers.cpp
  server/peer_handlers.cpp
  server/room_handlers.cpp
)
target_link_libraries(buzz_server_lib PUBLIC buzz_core)

add_executable(buzz-server server/main.cpp)
target_link_libraries(buzz-server PRIVATE buzz_server_lib)

add_library(buzz_client_lib
  client/client.cpp
  client/commands.cpp
  client/dm_queue.cpp
  client/peers.cpp
  client/room_state.cpp
  client/server_link.cpp
  client/server_messages.cpp
)
target_include_directories(buzz_client_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/client)
target_link_libraries(buzz_client_lib PUBLIC buzz_core)

add_executable(buzz client/main.cpp)
target_link_libraries(buzz PRIVATE buzz_client_lib)

add_executable(buzz-bench
  bench/bench_client.cpp
  bench/chat.cpp
  bench/main.cpp
  bench/scenarios.cpp
)
target_link_libraries(buzz-bench PRIVATE buzz_core)

install(TARGETS buzz-server buzz buzz-bench RUNTIME DESTINATION bin)
