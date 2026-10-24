cmake_minimum_required(VERSION 3.16)
project(buzz VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUZZ_BUILD_TESTS "Build the GoogleTest suites" ON)
option(BUZZ_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(BUZZ_WERROR "Treat compiler warnings as errors" OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "Buzz uses epoll and builds only on Linux. Use the Docker image on other systems.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
if(BUZZ_WERROR)
  add_compile_options(-Werror)
endif()

if(BUZZ_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(src)

if(BUZZ_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
