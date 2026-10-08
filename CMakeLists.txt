cmake_minimum_required(VERSION 3.16)
project(asicen_userland VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ASICEN_ENABLE_LIBUSB "Build libusb transport/probe" ON)
option(ASICEN_BUILD_TESTS "Build offline protocol tests" ON)

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
target_compile_options(px4_portable_reference PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Product profile compiles the same portable lifecycle with ASICEN's local
# receiver mapping, product directory, and wire magic.
add_library(asicen_px4_mock STATIC ${ASICEN_PX4_CORE_SOURCES}
  userland/src/px4_mock_backend.cpp)
target_include_directories(asicen_px4_mock PUBLIC
  ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/src userland/include)
target_compile_definitions(asicen_px4_mock PRIVATE ASICEN_PROFILE_ASICEN=1)
target_link_libraries(asicen_px4_mock PUBLIC Threads::Threads)
target_compile_options(asicen_px4_mock PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(asicen_core STATIC
  userland/src/card_mailbox.cpp
  userland/src/channel_plan.cpp
  userland/src/device_profile.cpp
  userland/src/enclosure.cpp
  userland/src/enclosure_grouping.cpp
  userland/src/frontend_facts.cpp
  userland/src/frontend_sequence.cpp
  userland/src/ipc.cpp
  userland/src/loader_firmware.cpp
  userland/src/link_seed_state.cpp
  userland/src/mock_backend.cpp
  userland/src/protocol.cpp
  userland/src/queued_capture.cpp
  userland/src/receiver_service.cpp
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
    ${ASICEN_PX4_ROOT}/tools/px4_ts_core.cpp
    ${ASICEN_PX4_ROOT}/tools/px4_ts_posix.cpp
    ${ASICEN_PX4_ROOT}/tools/px4ctl_format.cpp
    ${ASICEN_PX4_ROOT}/tools/px4d_list_format.cpp
    ${ASICEN_PX4_ROOT}/tests/control_integration_tests.cpp
    ${ASICEN_PX4_ROOT}/tests/control_workers_tests.cpp
    ${ASICEN_PX4_ROOT}/tests/card_service_tests.cpp
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
    PX4_POSIX_IPC_TEST_ACCESS=1)
  target_link_libraries(px4-portable-tests PRIVATE Threads::Threads)
  target_compile_options(px4-portable-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME px4-portable-tests COMMAND px4-portable-tests)
  set_tests_properties(px4-portable-tests PROPERTIES TIMEOUT 60)

  add_executable(asicen-product-profile-tests userland/tests/product_profile_tests.cpp)
  target_include_directories(asicen-product-profile-tests PRIVATE userland/include)
  target_compile_options(asicen-product-profile-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-product-profile-tests COMMAND asicen-product-profile-tests)

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

  add_executable(asicen-link-seed-state-tests userland/tests/link_seed_state_tests.cpp)
  target_link_libraries(asicen-link-seed-state-tests PRIVATE asicen_core)
  target_compile_options(asicen-link-seed-state-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME asicen-link-seed-state-tests COMMAND asicen-link-seed-state-tests)

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
