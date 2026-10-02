add_executable(bench_env bench_env.c)
target_link_libraries(bench_env PRIVATE xr)

add_executable(bench_roundtrip bench_roundtrip.c)
target_link_libraries(bench_roundtrip PRIVATE xr)

add_executable(bench_fanin bench_fanin.c)
target_link_libraries(bench_fanin PRIVATE xr)

add_executable(bench_echo bench_echo.c)
target_link_libraries(bench_echo PRIVATE xr)

add_executable(bench_l0 bench_l0.c)
target_link_libraries(bench_l0 PRIVATE xr)
