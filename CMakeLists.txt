cmake_minimum_required(VERSION 3.16)
project(bufpool CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(BUFPOOL_BUILD_TESTS "Build unit tests" ON)
option(BUFPOOL_BUILD_BENCH "Build the trace replay benchmark" ON)
set(BUFPOOL_SANITIZE "" CACHE STRING "Sanitizer to enable (address, thread, ...)")

if(BUFPOOL_SANITIZE)
  add_compile_options(-fsanitize=${BUFPOOL_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${BUFPOOL_SANITIZE})
endif()

find_package(Threads REQUIRED)

add_library(bufpool src/pool.cc)
target_include_directories(bufpool PUBLIC include)
target_compile_options(bufpool PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(bufpool PUBLIC Threads::Threads)

if(BUFPOOL_BUILD_BENCH)
  add_executable(bufpool_bench bench/replay.cc)
  target_link_libraries(bufpool_bench PRIVATE bufpool)
endif()

if(BUFPOOL_BUILD_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  add_executable(bufpool_tests
    tests/basic_test.cc
    tests/budget_test.cc
    tests/threads_debug_test.cc)
  target_link_libraries(bufpool_tests PRIVATE bufpool GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(bufpool_tests)
endif()
