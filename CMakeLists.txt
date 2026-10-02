cmake_minimum_required(VERSION 3.16)
project(xruntime C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(XR_BUILD_TESTS "Build unit tests" ON)
option(XR_BUILD_BENCH "Build benchmarks" ON)
set(XR_SANITIZE "none" CACHE STRING "none|address|thread|undefined")

# 平台:仅 Linux 沙盒(epoll/eventfd/futex/sched_*);不做跨平台兼容层。
set(XR_WARN_FLAGS -Wall -Wextra -Werror -Wshadow -Wpointer-arith
                  -Wstrict-prototypes -Wmissing-prototypes)

find_package(Threads REQUIRED)

add_library(xr STATIC
  src/xr_time.c
  src/xr_log.c
  src/xr_env.c
  src/xr_parker.c
)
target_include_directories(xr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(xr PRIVATE ${XR_WARN_FLAGS})
target_link_libraries(xr PUBLIC Threads::Threads)

if(XR_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address)
elseif(XR_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
elseif(XR_SANITIZE STREQUAL "undefined")
  add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all -g)
  add_link_options(-fsanitize=undefined)
elseif(NOT XR_SANITIZE STREQUAL "none")
  message(FATAL_ERROR "XR_SANITIZE must be none|address|thread|undefined")
endif()

if(XR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(XR_BUILD_BENCH)
  add_subdirectory(bench)
endif()
