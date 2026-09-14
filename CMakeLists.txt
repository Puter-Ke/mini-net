cmake_minimum_required(VERSION 3.16)
project(mini_net CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -g)

option(ENABLE_ASAN "Enabled AddressSanitizer + UBSan" OFF)
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# 核心库。规矩：每写完一个模块，把它对应的 .cc 加到这里（不要用 file(GLOB)）。
# M1 已完成：EventLoop / EpollPoller / Channel / TcpServer
# 待加入：# src/ThreadPool.cc   src/HttpParser.cc   src/MemoryPool.cc
add_library(mininet STATIC
  src/EventLoop.cc
  src/EpollPoller.cc
  src/Channel.cc
  src/TcpServer.cc
  src/Buffer.cc
  src/TimerWheel.cc
)
target_include_directories(mininet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mininet PUBLIC pthread)

add_executable(mini_net_server src/main.cc)
target_link_libraries(mini_net_server PRIVATE mininet)

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)
