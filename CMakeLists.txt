cmake_minimum_required(VERSION 3.16)
project(asicen_userland VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ASICEN_ENABLE_LIBUSB "Build libusb transport/probe" ON)
option(ASICEN_ENABLE_IFD "Build the host-libc PC/SC IFD plugin" ON)
option(ASICEN_REQUIRE_IFD "Fail configuration if the requested IFD dependencies are missing" OFF)
option(ASICEN_BUILD_TESTS "Build offline protocol tests" ON)
set(ASICEN_LIBUSB_INCLUDE_DIR "" CACHE PATH "libusb include directory (overrides pkg-config)")
set(ASICEN_LIBUSB_LIBRARY "" CACHE FILEPATH "libusb library/archive (overrides pkg-config)")
set(ASICEN_LIBUSB_EXTRA_LIBRARIES "" CACHE STRING "Additional private libraries required by the selected libusb archive")
set(ASICEN_LIBUSB_EXTRA_LINK_OPTIONS "" CACHE STRING "Additional linker options required by the selected libusb archive")

find_package(Threads REQUIRED)

set(ASICEN_PX4_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/px4-userland/userland)
set(ASICEN_PX4_CORE_SOURCES
    ${ASICEN_PX4_ROOT}/src/error.cpp
    ${ASICEN_PX4_ROOT}/src/identity.cpp
  ${ASICEN_PX4_ROOT}/src/ipc.cpp
  ${ASICEN_PX4_ROOT}/src/posix_ipc.cpp
  ${ASICEN_PX4_ROOT}/src/control_client.cpp
  ${ASICEN_PX4_ROOT}/src/control_server.cpp
  ${ASICEN_PX4_ROOT}/src/control_workers.cpp
  ${ASICEN_PX4_ROOT}/src/posix_tuner_nonce.cpp
  ${ASICEN_PX4_ROOT}/src/tuner_service.cpp
  ${ASICEN_PX4_ROOT}/src/card_service.cpp
)

# The reference profile is built unchanged for its original lifecycle tests.
add_library(px4_portable_reference STATIC ${ASICEN_PX4_CORE_SOURCES})
target_include_directories(px4_portable_reference PUBLIC
  ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/src)
target_link_libraries(px4_portable_reference PUBLIC Threads::Threads)
target_compile_definitions(px4_portable_reference PUBLIC
  PX4_USERLAND_DISABLE_NATIVE_CARD_SESSION=1)
target_compile_options(px4_portable_reference PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Product profile compiles the same portable lifecycle with ASICEN's local
# receiver mapping, product directory, and wire magic.
add_library(asicen_px4_mock STATIC ${ASICEN_PX4_CORE_SOURCES}
  ${ASICEN_PX4_ROOT}/src/card.cpp
  userland/src/px4_mock_backend.cpp)
target_include_directories(asicen_px4_mock PUBLIC
  ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/src userland/include)
target_compile_definitions(asicen_px4_mock PRIVATE ASICEN_PROFILE_ASICEN=1)
target_compile_definitions(asicen_px4_mock PUBLIC
  PX4_USERLAND_DISABLE_NATIVE_CARD_SESSION=1)
target_link_libraries(asicen_px4_mock PUBLIC Threads::Threads)
target_compile_options(asicen_px4_mock PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(asicen_px4_mock PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(asicen_hardware_session STATIC
  userland/src/hardware_stream_session.cpp)
target_include_directories(asicen_hardware_session PUBLIC
  userland/include ${ASICEN_PX4_ROOT}/include)
target_link_libraries(asicen_hardware_session PUBLIC asicen_px4_mock Threads::Threads)
target_compile_options(asicen_hardware_session PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(asicen_core STATIC
  userland/src/capture_drain.cpp
  userland/src/card_mailbox.cpp
  userland/src/channel_plan.cpp
  userland/src/device_profile.cpp
  userland/src/enclosure.cpp
  userland/src/enclosure_grouping.cpp
  userland/src/enclosure_lock.cpp
  userland/src/frontend_facts.cpp
  userland/src/frontend_sequence.cpp
  userland/src/hardware_gpio_guard.cpp
  userland/src/hardware_ownership.cpp
  userland/src/ipc.cpp
  userland/src/loader_firmware.cpp
  userland/src/link_seed_diagnostic.cpp
  userland/src/mock_backend.cpp
  userland/src/protocol.cpp
  userland/src/queued_capture.cpp
  userland/src/receiver_service.cpp
  userland/src/satellite_tune.cpp
  userland/src/secure_entropy.cpp
  userland/src/stream_capture.cpp
  userland/src/stream_queue.cpp
  userland/src/ts_framer.cpp
  userland/src/transport_transform.cpp
  userland/src/transport_capture.cpp
  userland/src/write_protocol.cpp
)
target_include_directories(asicen_core PUBLIC userland/include)
target_link_libraries(asicen_core PUBLIC Threads::Threads)
target_compile_options(asicen_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(ASICEN_ENABLE_LIBUSB)
  if(ASICEN_LIBUSB_INCLUDE_DIR OR ASICEN_LIBUSB_LIBRARY)
    if(NOT ASICEN_LIBUSB_INCLUDE_DIR OR NOT ASICEN_LIBUSB_LIBRARY)
      message(FATAL_ERROR "ASICEN_LIBUSB_INCLUDE_DIR and ASICEN_LIBUSB_LIBRARY must be set together")
    endif()
    if(NOT EXISTS "${ASICEN_LIBUSB_INCLUDE_DIR}/libusb.h")
      message(FATAL_ERROR "libusb.h not found under ASICEN_LIBUSB_INCLUDE_DIR")
    endif()
    if(NOT EXISTS "${ASICEN_LIBUSB_LIBRARY}")
      message(FATAL_ERROR "ASICEN_LIBUSB_LIBRARY does not exist: ${ASICEN_LIBUSB_LIBRARY}")
    endif()
    add_library(asicen_libusb_dependency UNKNOWN IMPORTED GLOBAL)
    set_target_properties(asicen_libusb_dependency PROPERTIES
      IMPORTED_LOCATION "${ASICEN_LIBUSB_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${ASICEN_LIBUSB_INCLUDE_DIR}")
    set(ASICEN_LIBUSB_TARGET asicen_libusb_dependency)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
    set(ASICEN_LIBUSB_TARGET PkgConfig::LIBUSB)
  endif()

  add_library(asicen_libusb STATIC
    userland/src/libusb_transport.cpp
    userland/src/libusb_hardware_backend.cpp
    userland/src/card_mailbox_hardware.cpp
    userland/src/card_only_service.cpp
  )
  target_include_directories(asicen_libusb PUBLIC userland/include)
  target_link_libraries(asicen_libusb PUBLIC asicen_core asicen_hardware_session
    ${ASICEN_LIBUSB_TARGET} ${ASICEN_LIBUSB_EXTRA_LIBRARIES})
  target_link_options(asicen_libusb INTERFACE ${ASICEN_LIBUSB_EXTRA_LINK_OPTIONS})
  target_compile_options(asicen_libusb PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(UNIX)
  add_library(asicen_pcsc_ifd_core STATIC
    third_party/px4-userland/userland/src/pcsc_ifd_adapter.cpp)
  set_target_properties(asicen_pcsc_ifd_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
  target_include_directories(asicen_pcsc_ifd_core PUBLIC
    third_party/px4-userland/userland/include)
  target_compile_definitions(asicen_pcsc_ifd_core PRIVATE
    ASICEN_PCSC_DEVICE_PREFIX="asicen-userland:")
  target_link_libraries(asicen_pcsc_ifd_core PUBLIC asicen_px4_mock)
  target_compile_options(asicen_pcsc_ifd_core PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

  if(ASICEN_BUILD_TESTS)
    add_executable(asicen-ifd-prefix-tests
      userland/tests/asicen_ifd_prefix_tests.cpp)
    target_link_libraries(asicen-ifd-prefix-tests PRIVATE asicen_pcsc_ifd_core)
    target_compile_options(asicen-ifd-prefix-tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()

  if(ASICEN_ENABLE_IFD)
    if(NOT APPLE)
      find_package(PkgConfig QUIET)
      if(PkgConfig_FOUND)
        pkg_check_modules(PCSCLITE QUIET IMPORTED_TARGET libpcsclite)
      endif()
    endif()
    if(APPLE)
      find_path(ASICEN_PCSC_IFD_INCLUDE_DIR
        NAMES ifdhandler.h
        HINTS "${CMAKE_OSX_SYSROOT}/System/Library/Frameworks/PCSC.framework/Headers"
        PATH_SUFFIXES PCSC PCSC.framework/Headers)
      set(ASICEN_PCSC_AVAILABLE ${ASICEN_PCSC_IFD_INCLUDE_DIR})
    else()
      set(ASICEN_PCSC_AVAILABLE ${PCSCLITE_FOUND})
      set(ASICEN_PCSC_IFD_INCLUDE_DIR ${PCSCLITE_INCLUDE_DIRS})
    endif()
    if(ASICEN_PCSC_AVAILABLE)
      add_library(asicen-ifdhandler SHARED
        third_party/px4-userland/userland/src/pcsc_ifd.cpp)
      target_include_directories(asicen-ifdhandler PRIVATE
        third_party/px4-userland/userland/include ${ASICEN_PCSC_IFD_INCLUDE_DIR})
      if(APPLE)
        target_link_libraries(asicen-ifdhandler PRIVATE asicen_pcsc_ifd_core)
      else()
        target_link_libraries(asicen-ifdhandler PRIVATE
          asicen_pcsc_ifd_core PkgConfig::PCSCLITE)
      endif()
      target_compile_options(asicen-ifdhandler PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
      set_target_properties(asicen-ifdhandler PROPERTIES OUTPUT_NAME ifd-asicen)
      if(APPLE)
        target_link_options(asicen-ifdhandler PRIVATE
          "LINKER:-install_name,@rpath/libifd-asicen.dylib")
        set(ASICEN_IFD_BUNDLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ASICEN-IFD.bundle")
        set(ASICEN_IFD_BUNDLE_EXECUTABLE
          "${CMAKE_SHARED_LIBRARY_PREFIX}ifd-asicen${CMAKE_SHARED_LIBRARY_SUFFIX}")
        set(ASICEN_IFD_BUNDLE_PLIST "${CMAKE_CURRENT_BINARY_DIR}/ASICEN-IFD-Info.plist")
        configure_file(packaging/pcsc/macos/Info.plist.in
          "${ASICEN_IFD_BUNDLE_PLIST}" @ONLY)
        add_custom_command(OUTPUT
            "${ASICEN_IFD_BUNDLE_DIR}/Contents/Info.plist"
            "${ASICEN_IFD_BUNDLE_DIR}/Contents/MacOS/${ASICEN_IFD_BUNDLE_EXECUTABLE}"
          COMMAND ${CMAKE_COMMAND} -E make_directory
            "${ASICEN_IFD_BUNDLE_DIR}/Contents/MacOS"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ASICEN_IFD_BUNDLE_PLIST}"
            "${ASICEN_IFD_BUNDLE_DIR}/Contents/Info.plist"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:asicen-ifdhandler>"
            "${ASICEN_IFD_BUNDLE_DIR}/Contents/MacOS/${ASICEN_IFD_BUNDLE_EXECUTABLE}"
          DEPENDS asicen-ifdhandler "${ASICEN_IFD_BUNDLE_PLIST}"
          VERBATIM)
        add_custom_target(asicen-ifd-bundle ALL DEPENDS
          "${ASICEN_IFD_BUNDLE_DIR}/Contents/Info.plist"
          "${ASICEN_IFD_BUNDLE_DIR}/Contents/MacOS/${ASICEN_IFD_BUNDLE_EXECUTABLE}")
        set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES
          "${ASICEN_IFD_BUNDLE_DIR}")
        target_link_options(asicen-ifdhandler PRIVATE
          "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/scripts/ifd.exports.macos")
      endif()
    elseif(ASICEN_REQUIRE_IFD)
      message(FATAL_ERROR "ASICEN_ENABLE_IFD=ON requires native PC/SC IFD headers")
    else()
      message(STATUS "native PC/SC IFD headers unavailable; skipping asicen-ifdhandler")
    endif()
  endif()
endif()

if(ASICEN_ENABLE_LIBUSB)
  add_executable(asicen-probe userland/tools/asicen_probe.cpp)
  target_link_libraries(asicen-probe PRIVATE asicen_libusb)
  target_compile_options(asicen-probe PRIVATE -Wall -Wextra -Wpedantic -Werror)

  add_executable(asicen-frontend userland/tools/asicen_frontend.cpp)
  target_link_libraries(asicen-frontend PRIVATE asicen_libusb)
  target_compile_options(asicen-frontend PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(UNIX)
  add_executable(asicen-transform userland/tools/asicen_transform.cpp)
  target_link_libraries(asicen-transform PRIVATE asicen_core)
  target_compile_options(asicen-transform PRIVATE -Wall -Wextra -Wpedantic -Werror)

  add_executable(asicend
    userland/tools/asicend.cpp
    userland/tools/asicend_research.cpp)
  target_link_libraries(asicend PRIVATE asicen_px4_mock asicen_core Threads::Threads)
  target_compile_options(asicend PRIVATE -Wall -Wextra -Wpedantic -Werror)

  if(ASICEN_ENABLE_LIBUSB)
    target_sources(asicend PRIVATE userland/tools/asicend_hardware.cpp)
    target_compile_definitions(asicend PRIVATE ASICEN_ENABLE_LIBUSB=1)
    target_link_libraries(asicend PRIVATE asicen_libusb)
  endif()

  add_executable(asicenctl
    userland/tools/asicenctl.cpp
    ${ASICEN_PX4_ROOT}/tools/px4ctl_format.cpp)
  target_include_directories(asicenctl PRIVATE
    ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/tools)
  target_compile_definitions(asicenctl PRIVATE ASICEN_PRODUCT_CLI=1)
  target_link_libraries(asicenctl PRIVATE asicen_px4_mock)
  target_compile_options(asicenctl PRIVATE -Wall -Wextra -Wpedantic -Werror)

  add_executable(asicen-ts
    userland/tools/asicen_ts.cpp
    userland/tools/asicen_ts_research.cpp
    userland/tools/asicen_ts_portable.cpp
    ${ASICEN_PX4_ROOT}/tools/px4_ts_core.cpp
    ${ASICEN_PX4_ROOT}/tools/px4_ts_posix.cpp)
  target_include_directories(asicen-ts PRIVATE
    ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/tools)
  target_compile_definitions(asicen-ts PRIVATE ASICEN_PRODUCT_CLI=1)
  target_link_libraries(asicen-ts PRIVATE asicen_core asicen_px4_mock)
  target_compile_options(asicen-ts PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(ASICEN_BUILD_TESTS)
  enable_testing()

  add_executable(px4-portable-tests
    ${ASICEN_PX4_CORE_SOURCES}
    ${ASICEN_PX4_ROOT}/src/card.cpp
    ${ASICEN_PX4_ROOT}/tools/px4_ts_core.cpp
    ${ASICEN_PX4_ROOT}/tools/px4_ts_posix.cpp
    ${ASICEN_PX4_ROOT}/tools/px4ctl_format.cpp
    ${ASICEN_PX4_ROOT}/tools/px4d_list_format.cpp
    ${ASICEN_PX4_ROOT}/tests/control_integration_tests.cpp
    ${ASICEN_PX4_ROOT}/tests/control_workers_tests.cpp
    ${ASICEN_PX4_ROOT}/tests/card_service_tests.cpp
    ${ASICEN_PX4_ROOT}/tests/card_tests.cpp
    ${ASICEN_PX4_ROOT}/tests/tuner_service_tests.cpp
    ${ASICEN_PX4_ROOT}/tests/ipc_state_tests.cpp
    ${ASICEN_PX4_ROOT}/tests/posix_ipc_tests.cpp
    ${ASICEN_PX4_ROOT}/tests/posix_tuner_nonce_tests.cpp
    ${ASICEN_PX4_ROOT}/tests/px4_ts_tests.cpp
    ${ASICEN_PX4_ROOT}/tests/px4ctl_format_tests.cpp
    ${ASICEN_PX4_ROOT}/tests/px4d_list_format_tests.cpp
    ${ASICEN_PX4_ROOT}/tests/imported_tests_main.cpp
  )
  target_include_directories(px4-portable-tests PRIVATE
    ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/src
    ${ASICEN_PX4_ROOT}/tools ${ASICEN_PX4_ROOT}/tests)
  target_compile_definitions(px4-portable-tests PRIVATE
    PX4_CONTROL_WORKERS_TEST_ACCESS=1
    PX4_CONTROL_SERVER_TEST_ACCESS=1
    PX4_POSIX_IPC_TEST_ACCESS=1
    PX4_USERLAND_DISABLE_NATIVE_CARD_SESSION=1)
  target_link_libraries(px4-portable-tests PRIVATE Threads::Threads)
  target_compile_options(px4-portable-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME px4-portable-tests COMMAND px4-portable-tests)

  if(UNIX)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(PCSCLITE_TEST QUIET IMPORTED_TARGET libpcsclite)
    endif()
    if(PCSCLITE_TEST_FOUND)
      add_executable(px4-pcsc-ifd-tests
        third_party/px4-userland/userland/src/pcsc_ifd_adapter.cpp
        third_party/px4-userland/userland/src/pcsc_ifd.cpp
        third_party/px4-userland/userland/tests/pcsc_ifd_tests.cpp)
      target_include_directories(px4-pcsc-ifd-tests PRIVATE
        third_party/px4-userland/userland/include
        third_party/px4-userland/userland/tests)
      target_link_libraries(px4-pcsc-ifd-tests PRIVATE
        px4_portable_reference PkgConfig::PCSCLITE_TEST Threads::Threads)
      target_compile_options(px4-pcsc-ifd-tests PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
      add_test(NAME px4-pcsc-ifd-tests COMMAND px4-pcsc-ifd-tests)
      set_tests_properties(px4-pcsc-ifd-tests PROPERTIES TIMEOUT 20)
      if(TARGET asicen-ifd-prefix-tests)
        add_test(NAME asicen-ifd-prefix-tests COMMAND asicen-ifd-prefix-tests)
      endif()
      if(ASICEN_ENABLE_LIBUSB)
        add_executable(asicen-card-only-service-tests
          userland/tests/card_only_service_tests.cpp)
        target_include_directories(asicen-card-only-service-tests PRIVATE
          third_party/px4-userland/userland/include)
        target_link_libraries(asicen-card-only-service-tests PRIVATE
          asicen_libusb asicen_pcsc_ifd_core)
        target_compile_options(asicen-card-only-service-tests PRIVATE
          -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME asicen-card-only-service-tests
          COMMAND asicen-card-only-service-tests)
        set_tests_properties(asicen-card-only-service-tests PROPERTIES TIMEOUT 20)
      endif()
    endif()
  endif()
  if(APPLE AND TARGET asicen-ifd-prefix-tests)
    add_test(NAME asicen-ifd-prefix-tests-macos COMMAND asicen-ifd-prefix-tests)
  endif()
  set_tests_properties(px4-portable-tests PROPERTIES TIMEOUT 60)

  add_executable(asicen-product-profile-tests userland/tests/product_profile_tests.cpp)
  target_include_directories(asicen-product-profile-tests PRIVATE userland/include)
  target_compile_options(asicen-product-profile-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-product-profile-tests COMMAND asicen-product-profile-tests)

  add_executable(asicen-hardware-ownership-tests
    userland/tests/hardware_ownership_tests.cpp
    userland/src/hardware_ownership.cpp)
  target_include_directories(asicen-hardware-ownership-tests PRIVATE userland/include)
  target_compile_options(asicen-hardware-ownership-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-hardware-ownership-tests COMMAND asicen-hardware-ownership-tests)

  if(ASICEN_ENABLE_LIBUSB)
    add_executable(asicen-libusb-capture-lifecycle-tests
      userland/tests/libusb_capture_lifecycle_tests.cpp)
    target_link_libraries(asicen-libusb-capture-lifecycle-tests PRIVATE asicen_libusb)
    target_compile_options(asicen-libusb-capture-lifecycle-tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME asicen-libusb-capture-lifecycle-tests
      COMMAND asicen-libusb-capture-lifecycle-tests)
    set_tests_properties(asicen-libusb-capture-lifecycle-tests PROPERTIES TIMEOUT 20)
  endif()

  add_executable(asicen-enclosure-lock-tests
    userland/tests/enclosure_lock_tests.cpp)
  target_include_directories(asicen-enclosure-lock-tests PRIVATE userland/include)
  target_link_libraries(asicen-enclosure-lock-tests PRIVATE asicen_core)
  target_compile_options(asicen-enclosure-lock-tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-enclosure-lock-tests COMMAND asicen-enclosure-lock-tests)

  add_executable(asicen-hardware-session-tests
    userland/tests/hardware_stream_session_tests.cpp)
  target_include_directories(asicen-hardware-session-tests PRIVATE
    userland/include ${ASICEN_PX4_ROOT}/include)
  target_link_libraries(asicen-hardware-session-tests PRIVATE asicen_hardware_session)
  target_compile_options(asicen-hardware-session-tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-hardware-session-tests COMMAND asicen-hardware-session-tests)
  set_tests_properties(asicen-hardware-session-tests PROPERTIES TIMEOUT 10)

  add_executable(asicen-hardware-ipc-integration-tests
    userland/tests/hardware_ipc_integration_tests.cpp)
  target_include_directories(asicen-hardware-ipc-integration-tests PRIVATE
    userland/include ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/tests)
  target_link_libraries(asicen-hardware-ipc-integration-tests PRIVATE asicen_hardware_session)
  target_compile_options(asicen-hardware-ipc-integration-tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-hardware-ipc-integration-tests
           COMMAND asicen-hardware-ipc-integration-tests)
  set_tests_properties(asicen-hardware-ipc-integration-tests PROPERTIES TIMEOUT 10)

  add_executable(asicen-capture-drain-tests userland/tests/capture_drain_tests.cpp)
  target_include_directories(asicen-capture-drain-tests PRIVATE userland/include)
  target_link_libraries(asicen-capture-drain-tests PRIVATE asicen_core)
  target_compile_options(asicen-capture-drain-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-capture-drain-tests COMMAND asicen-capture-drain-tests)
  set_tests_properties(asicen-capture-drain-tests PROPERTIES TIMEOUT 10)

  add_executable(asicen-secure-entropy-tests
    userland/tests/secure_entropy_tests.cpp userland/src/secure_entropy.cpp)
  target_include_directories(asicen-secure-entropy-tests PRIVATE userland/include)
  target_compile_options(asicen-secure-entropy-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-secure-entropy-tests COMMAND asicen-secure-entropy-tests)

  add_executable(asicen-hardware-gpio-guard-tests
    userland/tests/hardware_gpio_guard_tests.cpp)
  target_include_directories(asicen-hardware-gpio-guard-tests PRIVATE userland/include)
  target_link_libraries(asicen-hardware-gpio-guard-tests PRIVATE asicen_core)
  target_compile_options(asicen-hardware-gpio-guard-tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-hardware-gpio-guard-tests COMMAND asicen-hardware-gpio-guard-tests)

  add_executable(asicen-gpio-snapshot-tests
    userland/tests/gpio_snapshot_tests.cpp)
  target_include_directories(asicen-gpio-snapshot-tests PRIVATE userland/include)
  target_compile_options(asicen-gpio-snapshot-tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-gpio-snapshot-tests COMMAND asicen-gpio-snapshot-tests)

  add_executable(asicen-powered-init-sequence-tests
    userland/tests/powered_init_sequence_tests.cpp)
  target_include_directories(asicen-powered-init-sequence-tests PRIVATE userland/include)
  target_compile_options(asicen-powered-init-sequence-tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-powered-init-sequence-tests
           COMMAND asicen-powered-init-sequence-tests)

  add_executable(asicen-product-shutdown-tests
    userland/tests/product_shutdown_hooks_tests.cpp)
  target_include_directories(asicen-product-shutdown-tests PRIVATE
    userland/include ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/tests)
  target_link_libraries(asicen-product-shutdown-tests PRIVATE asicen_px4_mock)
  target_compile_options(asicen-product-shutdown-tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-product-shutdown-tests COMMAND asicen-product-shutdown-tests)
  set_tests_properties(asicen-product-shutdown-tests PROPERTIES TIMEOUT 10)

  add_executable(asicen-product-worker-routing-tests
    userland/tests/product_worker_routing_tests.cpp)
  target_include_directories(asicen-product-worker-routing-tests PRIVATE
    userland/include ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/tests)
  target_link_libraries(asicen-product-worker-routing-tests PRIVATE asicen_px4_mock)
  target_compile_options(asicen-product-worker-routing-tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-product-worker-routing-tests
    COMMAND asicen-product-worker-routing-tests)
  set_tests_properties(asicen-product-worker-routing-tests PROPERTIES TIMEOUT 10)

  if(UNIX)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME asicen-static-distribution-tests
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/userland/tests/static_distribution_tests.py)
    set_tests_properties(asicen-static-distribution-tests PROPERTIES TIMEOUT 20)
    add_test(NAME asicen-macos-distribution-tests
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/userland/tests/macos_distribution_tests.py)
    add_test(NAME asicen-linux-ifd-allowlist-tests
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/userland/tests/linux_ifd_allowlist_tests.py)
    add_test(NAME asicen-product-cli-integration
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/userland/tests/product_cli_integration.py
        $<TARGET_FILE:asicend> $<TARGET_FILE:asicenctl> $<TARGET_FILE:asicen-ts>)
    set_tests_properties(asicen-product-cli-integration PROPERTIES TIMEOUT 15)
  endif()

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

  add_executable(asicen-card-mailbox-hardware-tests
    userland/tests/card_mailbox_hardware_tests.cpp
    userland/src/card_mailbox_hardware.cpp)
  target_include_directories(asicen-card-mailbox-hardware-tests PRIVATE
    userland/include ${ASICEN_PX4_ROOT}/include)
  target_link_libraries(asicen-card-mailbox-hardware-tests PRIVATE
    asicen_core asicen_px4_mock Threads::Threads)
  target_compile_options(asicen-card-mailbox-hardware-tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-card-mailbox-hardware-tests
    COMMAND asicen-card-mailbox-hardware-tests)
  set_tests_properties(asicen-card-mailbox-hardware-tests PROPERTIES TIMEOUT 15)

  add_executable(asicen-loader-firmware-tests userland/tests/loader_firmware_tests.cpp)
  target_link_libraries(asicen-loader-firmware-tests PRIVATE asicen_core)
  target_compile_options(asicen-loader-firmware-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-loader-firmware-tests COMMAND asicen-loader-firmware-tests)

  add_executable(asicen-frontend-sequence-tests userland/tests/frontend_sequence_tests.cpp)
  target_link_libraries(asicen-frontend-sequence-tests PRIVATE asicen_core)
  target_compile_options(asicen-frontend-sequence-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-frontend-sequence-tests COMMAND asicen-frontend-sequence-tests)

  add_executable(asicen-shared-demod-tests userland/tests/shared_demod_tests.cpp)
  target_link_libraries(asicen-shared-demod-tests PRIVATE asicen_core)
  target_compile_options(asicen-shared-demod-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-shared-demod-tests COMMAND asicen-shared-demod-tests)

  add_executable(asicen-frontend-tune-tests userland/tests/frontend_tune_tests.cpp)
  target_link_libraries(asicen-frontend-tune-tests PRIVATE asicen_core)
  target_compile_options(asicen-frontend-tune-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-frontend-tune-tests COMMAND asicen-frontend-tune-tests)

  add_executable(asicen-frontend-gain-tests userland/tests/frontend_gain_tests.cpp)
  target_link_libraries(asicen-frontend-gain-tests PRIVATE asicen_core)
  target_compile_options(asicen-frontend-gain-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-frontend-gain-tests COMMAND asicen-frontend-gain-tests)

  add_executable(asicen-satellite-tune-tests userland/tests/satellite_tune_tests.cpp)
  target_link_libraries(asicen-satellite-tune-tests PRIVATE asicen_core)
  target_compile_options(asicen-satellite-tune-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-satellite-tune-tests COMMAND asicen-satellite-tune-tests)

  add_executable(asicen-stream-capture-tests userland/tests/stream_capture_tests.cpp)
  target_link_libraries(asicen-stream-capture-tests PRIVATE asicen_core)
  target_compile_options(asicen-stream-capture-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-stream-capture-tests COMMAND asicen-stream-capture-tests)

  add_executable(asicen-queued-capture-tests userland/tests/queued_capture_tests.cpp)
  target_link_libraries(asicen-queued-capture-tests PRIVATE asicen_core)
  target_compile_options(asicen-queued-capture-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-queued-capture-tests COMMAND asicen-queued-capture-tests)

  add_executable(asicen-stream-setup-tests userland/tests/stream_setup_tests.cpp)
  target_link_libraries(asicen-stream-setup-tests PRIVATE asicen_core)
  target_compile_options(asicen-stream-setup-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-stream-setup-tests COMMAND asicen-stream-setup-tests)

  add_executable(asicen-cf-chunk-address-tests userland/tests/cf_chunk_address_tests.cpp)
  target_link_libraries(asicen-cf-chunk-address-tests PRIVATE asicen_core)
  target_compile_options(asicen-cf-chunk-address-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-cf-chunk-address-tests COMMAND asicen-cf-chunk-address-tests)

  add_executable(asicen-capture-stdout-tests userland/tests/capture_stdout_tests.cpp)
  target_link_libraries(asicen-capture-stdout-tests PRIVATE asicen_core)
  target_compile_options(asicen-capture-stdout-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-capture-stdout-tests COMMAND asicen-capture-stdout-tests)

  add_executable(asicen-transport-transform-tests userland/tests/transport_transform_tests.cpp)
  target_link_libraries(asicen-transport-transform-tests PRIVATE asicen_core)
  target_compile_options(asicen-transport-transform-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-transport-transform-tests COMMAND asicen-transport-transform-tests)

  add_executable(asicen-link-seed-diagnostic-tests userland/tests/link_seed_diagnostic_tests.cpp)
  target_link_libraries(asicen-link-seed-diagnostic-tests PRIVATE asicen_core)
  target_compile_options(asicen-link-seed-diagnostic-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-link-seed-diagnostic-tests COMMAND asicen-link-seed-diagnostic-tests)

  add_executable(asicen-transport-capture-tests userland/tests/transport_capture_tests.cpp)
  target_link_libraries(asicen-transport-capture-tests PRIVATE asicen_core)
  target_compile_options(asicen-transport-capture-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-transport-capture-tests COMMAND asicen-transport-capture-tests)

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
