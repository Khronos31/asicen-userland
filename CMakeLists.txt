cmake_minimum_required(VERSION 3.16)
project(asicen_userland VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ASICEN_ENABLE_LIBUSB "Build libusb transport/probe" ON)
option(ASICEN_BUILD_TESTS "Build offline protocol tests" ON)

find_package(Threads REQUIRED)

add_library(asicen_core STATIC
  userland/src/card_mailbox.cpp
  userland/src/channel_plan.cpp
  userland/src/device_profile.cpp
  userland/src/enclosure.cpp
  userland/src/enclosure_grouping.cpp
  userland/src/frontend_facts.cpp
  userland/src/ipc.cpp
  userland/src/loader_firmware.cpp
  userland/src/mock_backend.cpp
  userland/src/protocol.cpp
  userland/src/receiver_service.cpp
  userland/src/stream_queue.cpp
  userland/src/ts_framer.cpp
  userland/src/write_protocol.cpp
)
target_include_directories(asicen_core PUBLIC userland/include)
target_link_libraries(asicen_core PUBLIC Threads::Threads)
target_compile_options(asicen_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(ASICEN_ENABLE_LIBUSB)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

  add_library(asicen_libusb STATIC
    userland/src/libusb_transport.cpp
  )
  target_include_directories(asicen_libusb PUBLIC userland/include)
  target_link_libraries(asicen_libusb PUBLIC asicen_core PkgConfig::LIBUSB)
  target_compile_options(asicen_libusb PRIVATE -Wall -Wextra -Wpedantic -Werror)

  add_executable(asicen-probe userland/tools/asicen_probe.cpp)
  target_link_libraries(asicen-probe PRIVATE asicen_libusb)
  target_compile_options(asicen-probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(UNIX)
  add_executable(asicend userland/tools/asicend.cpp)
  target_link_libraries(asicend PRIVATE asicen_core Threads::Threads)
  target_compile_options(asicend PRIVATE -Wall -Wextra -Wpedantic -Werror)

  add_executable(asicen-ts userland/tools/asicen_ts.cpp)
  target_link_libraries(asicen-ts PRIVATE asicen_core)
  target_compile_options(asicen-ts PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(ASICEN_BUILD_TESTS)
  enable_testing()

  add_executable(asicen-protocol-tests userland/tests/protocol_tests.cpp)
  target_link_libraries(asicen-protocol-tests PRIVATE asicen_core)
  target_compile_options(asicen-protocol-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-protocol-tests COMMAND asicen-protocol-tests)

  add_executable(asicen-write-protocol-tests userland/tests/write_protocol_tests.cpp)
  target_link_libraries(asicen-write-protocol-tests PRIVATE asicen_core)
  target_compile_options(asicen-write-protocol-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-write-protocol-tests COMMAND asicen-write-protocol-tests)

  add_executable(asicen-service-tests userland/tests/service_tests.cpp)
  target_link_libraries(asicen-service-tests PRIVATE asicen_core)
  target_compile_options(asicen-service-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-service-tests COMMAND asicen-service-tests)

  add_executable(asicen-ipc-tests userland/tests/ipc_tests.cpp)
  target_link_libraries(asicen-ipc-tests PRIVATE asicen_core)
  target_compile_options(asicen-ipc-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-ipc-tests COMMAND asicen-ipc-tests)

  add_executable(asicen-enclosure-grouping-tests userland/tests/enclosure_grouping_tests.cpp)
  target_link_libraries(asicen-enclosure-grouping-tests PRIVATE asicen_core)
  target_compile_options(asicen-enclosure-grouping-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-enclosure-grouping-tests COMMAND asicen-enclosure-grouping-tests)

  add_executable(asicen-ts-framer-tests userland/tests/ts_framer_tests.cpp)
  target_link_libraries(asicen-ts-framer-tests PRIVATE asicen_core)
  target_compile_options(asicen-ts-framer-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-ts-framer-tests COMMAND asicen-ts-framer-tests)

  add_executable(asicen-card-mailbox-tests userland/tests/card_mailbox_tests.cpp)
  target_link_libraries(asicen-card-mailbox-tests PRIVATE asicen_core)
  target_compile_options(asicen-card-mailbox-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-card-mailbox-tests COMMAND asicen-card-mailbox-tests)

  if(UNIX)
    add_test(
      NAME asicen-mock-integration
      COMMAND bash
        ${CMAKE_CURRENT_SOURCE_DIR}/userland/tests/mock_integration.sh
        $<TARGET_FILE:asicend>
        $<TARGET_FILE:asicen-ts>
    )
  endif()
endif()
