cmake_minimum_required(VERSION 3.16)
project(asicen_userland VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ASICEN_ENABLE_LIBUSB "Build libusb transport/probe" ON)

add_library(asicen_core STATIC
  userland/src/device_profile.cpp
  userland/src/protocol.cpp
)
target_include_directories(asicen_core PUBLIC userland/include)
target_compile_options(asicen_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(ASICEN_ENABLE_LIBUSB)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
  add_executable(asicen-probe userland/tools/asicen_probe.cpp)
  target_link_libraries(asicen-probe PRIVATE asicen_core PkgConfig::LIBUSB)
  target_compile_options(asicen-probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
