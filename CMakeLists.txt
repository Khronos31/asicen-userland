cmake_minimum_required(VERSION 3.16)
project(asicen_userland VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ASICEN_ENABLE_LIBUSB "Build libusb transport/probe" ON)
option(ASICEN_BUILD_TESTS "Build offline protocol tests" ON)

add_library(asicen_core STATIC
  userland/src/channel_plan.cpp
  userland/src/device_profile.cpp
  userland/src/frontend_facts.cpp
  userland/src/loader_firmware.cpp
  userland/src/protocol.cpp
)
target_include_directories(asicen_core PUBLIC userland/include)
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

if(ASICEN_BUILD_TESTS)
  enable_testing()
  add_executable(asicen-protocol-tests userland/tests/protocol_tests.cpp)
  target_link_libraries(asicen-protocol-tests PRIVATE asicen_core)
  target_compile_options(asicen-protocol-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-protocol-tests COMMAND asicen-protocol-tests)
endif()
