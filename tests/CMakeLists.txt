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

# L0 stackful 使用自定义栈,与 ASan/TSan 不兼容;仅在 sanitizer=none 注册。
if(XR_SANITIZE STREQUAL "none")
  add_executable(test_ctx test_ctx.c)
  target_link_libraries(test_ctx PRIVATE xr)
  add_test(NAME ctx COMMAND test_ctx)

  add_executable(test_ctx_migrate test_ctx_migrate.c)
  target_link_libraries(test_ctx_migrate PRIVATE xr)
  add_test(NAME ctx_migrate COMMAND test_ctx_migrate)

  add_executable(test_ctx_uffd test_ctx_uffd.c)
  target_link_libraries(test_ctx_uffd PRIVATE xr)
  add_test(NAME ctx_uffd COMMAND test_ctx_uffd)
endif()
