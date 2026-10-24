add_library(buzz_core
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
)
target_link_libraries(buzz_server_lib PUBLIC buzz_core)

add_executable(buzz-server server/main.cpp)
target_link_libraries(buzz-server PRIVATE buzz_server_lib)

add_executable(buzz
  client/main.cpp
  client/client.cpp
  client/commands.cpp
  client/peers.cpp
  client/server_link.cpp
)
target_link_libraries(buzz PRIVATE buzz_core)

install(TARGETS buzz-server buzz RUNTIME DESTINATION bin)
