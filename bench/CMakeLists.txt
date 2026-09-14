add_executable(echo_bench echo_bench.cc)
target_link_libraries(echo_bench PRIVATE pthread)
