include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
  GIT_SHALLOW TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(mininet_tests
  test_buffer.cc
  test_timer_wheel.cc
  test_event_loop.cc
  test_wakeup.cc
  test_thread_pool.cc
  test_idle_timeout.cc
  test_multithread_server.cc
)
target_link_libraries(mininet_tests PRIVATE mininet GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(mininet_tests)
