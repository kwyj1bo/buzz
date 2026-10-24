include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(buzz_tests
  crypto_test.cpp
  p2p_test.cpp
  peer_test.cpp
  protocol_test.cpp
  reliable_channel_test.cpp
  room_manager_test.cpp
  server_test.cpp
  persistence_test.cpp
  session_manager_test.cpp
  storage_test.cpp
  thread_pool_test.cpp
  user_store_test.cpp
  validate_test.cpp
  support/test_client.cpp
  support/test_server.cpp
)
target_include_directories(buzz_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(buzz_tests PRIVATE buzz_server_lib GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(buzz_tests DISCOVERY_TIMEOUT 60)
