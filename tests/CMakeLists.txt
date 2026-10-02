add_executable(test_smoke test_smoke.c)
target_link_libraries(test_smoke PRIVATE xr)
add_test(NAME smoke COMMAND test_smoke)

add_executable(test_parker test_parker.c)
target_link_libraries(test_parker PRIVATE xr)
add_test(NAME parker COMMAND test_parker)

add_executable(test_worker test_worker.c)
target_link_libraries(test_worker PRIVATE xr)
add_test(NAME worker COMMAND test_worker)

add_executable(test_mpsc test_mpsc.c)
target_link_libraries(test_mpsc PRIVATE xr)
add_test(NAME mpsc COMMAND test_mpsc)
