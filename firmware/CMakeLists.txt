cmake_minimum_required(VERSION 3.16)
project(ivpump_fw LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(IVPUMP_BUILD_TESTS "Build GoogleTest unit tests" ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(IVPUMP_WARNINGS -Wall -Wextra -Wshadow -Wdouble-promotion -Wformat=2 -Wno-format-truncation)
endif()

# --- Firmware (portable, hardware-agnostic; would run on the MCU) ----------
add_library(ivpump_fw STATIC
  src/logger.c
  src/monitor.c
  src/pump.c
  src/cmd.c
)
target_include_directories(ivpump_fw PUBLIC include)
target_compile_options(ivpump_fw PRIVATE ${IVPUMP_WARNINGS})
if(NOT MSVC)
  target_link_libraries(ivpump_fw PUBLIC m)
endif()

# --- Virtual test environment (plant, attacker, harness) -----------------
add_library(ivpump_simlib STATIC
  sim/src/plant.c
  sim/src/attack.c
  sim/src/sim.c
)
target_include_directories(ivpump_simlib PUBLIC sim/include)
target_link_libraries(ivpump_simlib PUBLIC ivpump_fw)
target_compile_options(ivpump_simlib PRIVATE ${IVPUMP_WARNINGS})

find_package(Threads REQUIRED)
add_executable(ivpump_sim sim/src/main.c)
target_link_libraries(ivpump_sim PRIVATE ivpump_simlib Threads::Threads)
target_compile_options(ivpump_sim PRIVATE ${IVPUMP_WARNINGS})
if(WIN32)
  target_link_libraries(ivpump_sim PRIVATE winmm)
endif()
if(MINGW)
  # Self-contained exe so the GUI can spawn it without MSYS2 DLLs on PATH.
  target_link_options(ivpump_sim PRIVATE -static)
endif()

# --- Tests ----------------------------------------------------------------
if(IVPUMP_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(ivpump_tests
    tests/test_logger.cpp
    tests/test_monitor.cpp
    tests/test_pump.cpp
    tests/test_cmd.cpp
    tests/test_attack.cpp
    tests/test_sim.cpp
  )
  target_include_directories(ivpump_tests PRIVATE tests)
  target_link_libraries(ivpump_tests PRIVATE ivpump_simlib GTest::gmock_main)

  include(GoogleTest)
  gtest_discover_tests(ivpump_tests DISCOVERY_MODE PRE_TEST)
endif()
