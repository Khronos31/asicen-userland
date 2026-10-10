# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ASICEN_VERSION_LINE)
string(LENGTH "${ASICEN_VERSION_LINE}" ASICEN_VERSION_LENGTH)
if(ASICEN_VERSION_LENGTH LESS 2)
    message(FATAL_ERROR "VERSION must contain exactly N.N.N followed by one newline")
endif()
math(EXPR ASICEN_VERSION_LAST_INDEX "${ASICEN_VERSION_LENGTH} - 1")
string(SUBSTRING "${ASICEN_VERSION_LINE}" ${ASICEN_VERSION_LAST_INDEX} 1 ASICEN_VERSION_TRAILING)
string(SUBSTRING "${ASICEN_VERSION_LINE}" 0 ${ASICEN_VERSION_LAST_INDEX} ASICEN_PROJECT_VERSION)
if(NOT ASICEN_VERSION_TRAILING STREQUAL "\n" OR
    ASICEN_PROJECT_VERSION MATCHES "[\r\n]" OR
    NOT ASICEN_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain exactly N.N.N followed by one newline")
endif()
project(asicen_userland VERSION "${ASICEN_PROJECT_VERSION}" LANGUAGES CXX)

option(ASICEN_BUILD_TESTS "Build offline portable-core tests" OFF)
option(ASICEN_ENABLE_LIBUSB "Build the native libusb transport" ON)
option(ASICEN_BUILD_TOOLS "Build developer-only USB probe tools" OFF)
option(ASICEN_BUILD_ANDROID_LINKCHECK "Build the Android-only final link check" OFF)
option(ASICEN_RELEASE_ASICEND_NO_BUILD_ID
    "Omit the GNU Build ID note from the release asicend executable only" OFF)
option(ASICEN_BUILD_PCSC_IFD "Build the POSIX PC/SC IFD Handler when headers are available" ON)
option(ASICEN_REQUIRE_PCSC_IFD "Fail configuration when the PC/SC IFD Handler cannot be built" OFF)
set(ASICEN_ANDROID_NDK_ROOT_MAP "" CACHE PATH
    "Android NDK root to remove from compiler debug paths")
set(ASICEN_STATIC_LINK_MAP_DIR "" CACHE PATH "Optional static release executable link maps")
set(ASICEN_ANDROID_LINK_MAP_DIR "" CACHE PATH
    "Optional output directory for Android production executable link maps")

if(WIN32)
    # Statically link the C++ runtime, unwinder, and winpthreads so release PEs
    # do not depend on toolchain runtime DLLs, and map machine-specific source
    # and build roots out of embedded strings. Applies to every target below.
    add_link_options(-static "-Wl,--no-insert-timestamp")
    add_compile_options(
        "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
        "-ffile-prefix-map=${CMAKE_BINARY_DIR}=."
    )
endif()

if(ANDROID)
    # Release ELFs must not retain the machine-specific source, build, or NDK
    # roots emitted by Clang's DWARF and macro expansion.  Keep the mapped
    # value relative so an unstripped developer build is reproducible too.
    set(asicen_android_prefix_map_flags
        "-fdebug-compilation-dir=."
        "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
        "-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=."
        "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=."
        "-ffile-prefix-map=${CMAKE_BINARY_DIR}=."
        "-fdebug-prefix-map=${CMAKE_BINARY_DIR}=."
        "-fmacro-prefix-map=${CMAKE_BINARY_DIR}=."
    )
    if(ASICEN_ANDROID_NDK_ROOT_MAP)
        list(APPEND asicen_android_prefix_map_flags
            "-ffile-prefix-map=${ASICEN_ANDROID_NDK_ROOT_MAP}=."
            "-fdebug-prefix-map=${ASICEN_ANDROID_NDK_ROOT_MAP}=."
            "-fmacro-prefix-map=${ASICEN_ANDROID_NDK_ROOT_MAP}=."
        )
    endif()
    add_compile_options(${asicen_android_prefix_map_flags})

    # Every Android executable/shared-library final link in this project inherits this
    # contract, including future daemons, CLIs, JNI libraries, and developer tools. Every
    # packaged ELF must additionally pass scripts/verify-android-elf.sh before publication.
    # Android builds use the same hardened PIE contract as the production probe.
    add_link_options(
        "-Wl,-z,relro"
        "-Wl,-z,now"
        "-Wl,-z,max-page-size=16384"
        "-Wl,-z,common-page-size=16384"
        "-Wl,--no-undefined"
    )
endif()

set(ASICEN_LIBUSB_INCLUDE_DIR "" CACHE PATH "libusb-1.0 include directory override")
set(ASICEN_LIBUSB_LIBRARY "" CACHE FILEPATH "libusb-1.0 library override")
set(ASICEN_LIBUSB_LINK_LIBRARIES "" CACHE STRING
    "Extra link items a static ASICEN_LIBUSB_LIBRARY needs (its pkg-config Libs.private)")

find_package(Threads REQUIRED)

if(ASICEN_ENABLE_LIBUSB)
    if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        # Preserve the reference discovery exception for FreeBSD's base
        # libusb without expanding ASICEN's supported platform scope.
        set(asicen_libusb_pkg_config_spec "libusb-1.0")
        set(asicen_libusb_requirement "libusb-1.0")
    else()
        set(asicen_libusb_pkg_config_spec "libusb-1.0>=1.0.23")
        set(asicen_libusb_requirement "libusb-1.0 >= 1.0.23")
    endif()
    find_package(PkgConfig QUIET)
    if(ASICEN_LIBUSB_INCLUDE_DIR AND ASICEN_LIBUSB_LIBRARY)
        add_library(asicen_libusb_dependency UNKNOWN IMPORTED)
        set_target_properties(asicen_libusb_dependency PROPERTIES
            IMPORTED_LOCATION "${ASICEN_LIBUSB_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ASICEN_LIBUSB_INCLUDE_DIR}"
        )
        set(ASICEN_LIBUSB_TARGET asicen_libusb_dependency)
    elseif(PkgConfig_FOUND)
        pkg_check_modules(PC_LIBUSB QUIET IMPORTED_TARGET
            "${asicen_libusb_pkg_config_spec}")
    endif()

    if(NOT ASICEN_LIBUSB_TARGET AND TARGET PkgConfig::PC_LIBUSB)
        set(ASICEN_LIBUSB_TARGET PkgConfig::PC_LIBUSB)
    elseif(NOT ASICEN_LIBUSB_TARGET)
        if(NOT ASICEN_LIBUSB_INCLUDE_DIR)
            find_path(ASICEN_LIBUSB_INCLUDE_DIR
                NAMES libusb.h
                PATH_SUFFIXES libusb-1.0
            )
        endif()
        if(NOT ASICEN_LIBUSB_LIBRARY)
            find_library(ASICEN_LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
        endif()
        if(NOT ASICEN_LIBUSB_INCLUDE_DIR OR NOT ASICEN_LIBUSB_LIBRARY)
            message(FATAL_ERROR
                "${asicen_libusb_requirement} is required; use pkg-config or set "
                "ASICEN_LIBUSB_INCLUDE_DIR and ASICEN_LIBUSB_LIBRARY")
        endif()
        add_library(asicen_libusb_dependency UNKNOWN IMPORTED)
        set_target_properties(asicen_libusb_dependency PROPERTIES
            IMPORTED_LOCATION "${ASICEN_LIBUSB_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ASICEN_LIBUSB_INCLUDE_DIR}"
        )
        set(ASICEN_LIBUSB_TARGET asicen_libusb_dependency)
    endif()
    if(TARGET asicen_libusb_dependency AND ASICEN_LIBUSB_LINK_LIBRARIES)
        separate_arguments(asicen_libusb_link_items UNIX_COMMAND
            "${ASICEN_LIBUSB_LINK_LIBRARIES}")
        set_property(TARGET asicen_libusb_dependency PROPERTY
            INTERFACE_LINK_LIBRARIES ${asicen_libusb_link_items})
    endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(asicen_enable_project_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -fno-exceptions
        -fno-rtti
    )
endfunction()

set(ASICEN_PX4_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/px4-userland/userland)
set(ASICEN_PX4_CORE_SOURCES
    ${ASICEN_PX4_ROOT}/src/error.cpp
    ${ASICEN_PX4_ROOT}/src/logging.cpp
    ${ASICEN_PX4_ROOT}/src/identity.cpp
    ${ASICEN_PX4_ROOT}/src/ipc.cpp
    ${ASICEN_PX4_ROOT}/src/control_client.cpp
    ${ASICEN_PX4_ROOT}/src/control_server.cpp
    ${ASICEN_PX4_ROOT}/src/control_workers.cpp
    ${ASICEN_PX4_ROOT}/src/tuner_service.cpp
    ${ASICEN_PX4_ROOT}/src/card_service.cpp
)

if(WIN32)
    list(APPEND ASICEN_PX4_CORE_SOURCES
        ${ASICEN_PX4_ROOT}/src/windows/windows_ipc.cpp
        ${ASICEN_PX4_ROOT}/src/windows/windows_tuner_nonce.cpp
        ${ASICEN_PX4_ROOT}/src/windows/windows_sleep.cpp
    )
else()
    list(APPEND ASICEN_PX4_CORE_SOURCES
        ${ASICEN_PX4_ROOT}/src/posix_ipc.cpp
        ${ASICEN_PX4_ROOT}/src/posix_tuner_nonce.cpp
    )
endif()

if(ASICEN_BUILD_TESTS)
    # The reference profile is built unchanged for its original lifecycle tests.
    add_library(px4_portable_reference STATIC ${ASICEN_PX4_CORE_SOURCES})
    target_include_directories(px4_portable_reference PUBLIC
        ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/src)
    target_link_libraries(px4_portable_reference PUBLIC Threads::Threads)
    target_compile_definitions(px4_portable_reference PUBLIC
        PX4_USERLAND_DISABLE_NATIVE_CARD_SESSION=1)
    asicen_enable_project_warnings(px4_portable_reference)
endif()

# Product profile compiles the same portable lifecycle with ASICEN's local
# receiver mapping, product directory, and wire magic.
add_library(asicen_portable_core STATIC ${ASICEN_PX4_CORE_SOURCES}
    ${ASICEN_PX4_ROOT}/src/card.cpp)
target_include_directories(asicen_portable_core PUBLIC
    ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/src userland/include)
target_compile_definitions(asicen_portable_core PRIVATE ASICEN_PROFILE_ASICEN=1)
target_compile_definitions(asicen_portable_core PUBLIC
    PX4_USERLAND_DISABLE_NATIVE_CARD_SESSION=1)
target_link_libraries(asicen_portable_core PUBLIC Threads::Threads)
asicen_enable_project_warnings(asicen_portable_core)
set_target_properties(asicen_portable_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Keep synthetic backends out of production archives and build targets.
if(ASICEN_BUILD_TESTS OR ASICEN_BUILD_TOOLS)
    add_library(asicen_px4_mock STATIC userland/src/px4_mock_backend.cpp)
    target_link_libraries(asicen_px4_mock PUBLIC asicen_portable_core)
    target_include_directories(asicen_px4_mock PUBLIC userland/include)
    asicen_enable_project_warnings(asicen_px4_mock)
endif()

add_library(asicen_hardware_session STATIC
    userland/src/hardware_stream_session.cpp)
target_include_directories(asicen_hardware_session PUBLIC
    userland/include ${ASICEN_PX4_ROOT}/include)
target_link_libraries(asicen_hardware_session PUBLIC asicen_portable_core Threads::Threads)
asicen_enable_project_warnings(asicen_hardware_session)

add_library(asicen_core STATIC
    userland/src/capture_drain.cpp
    userland/src/card_mailbox.cpp
    userland/src/channel_plan.cpp
    userland/src/device_profile.cpp
    userland/src/enclosure.cpp
    userland/src/enclosure_grouping.cpp
    userland/src/frontend_facts.cpp
    userland/src/firmware.cpp
    userland/src/frontend_sequence.cpp
    userland/src/v2_frontend.cpp
    userland/src/v2_nmi.cpp
    userland/src/hardware_gpio_guard.cpp
    userland/src/hardware_ownership.cpp
    userland/src/loader_firmware.cpp
    userland/src/link_seed_diagnostic.cpp
    userland/src/protocol.cpp
    userland/src/queued_capture.cpp
    userland/src/satellite_tune.cpp
    userland/src/stream_capture.cpp
    userland/src/ts_framer.cpp
    userland/src/transport_transform.cpp
    userland/src/transport_capture.cpp
    userland/src/write_protocol.cpp
)
target_include_directories(asicen_core PUBLIC userland/include ${ASICEN_PX4_ROOT}/include)
target_link_libraries(asicen_core PUBLIC Threads::Threads)
asicen_enable_project_warnings(asicen_core)

if(ASICEN_ENABLE_LIBUSB)
    add_library(asicen_libusb STATIC
        userland/src/libusb_transport.cpp
        userland/src/libusb_hardware_backend.cpp
        userland/src/card_mailbox_hardware.cpp
        userland/src/card_only_service.cpp
    )
    target_include_directories(asicen_libusb PUBLIC userland/include)
    target_link_libraries(asicen_libusb PUBLIC asicen_core asicen_hardware_session
        ${ASICEN_LIBUSB_TARGET})
    asicen_enable_project_warnings(asicen_libusb)
endif()

set(ASICEN_PCSC_IFD_AVAILABLE OFF)
if(ASICEN_REQUIRE_PCSC_IFD AND NOT ASICEN_BUILD_PCSC_IFD)
    message(FATAL_ERROR "ASICEN_REQUIRE_PCSC_IFD requires ASICEN_BUILD_PCSC_IFD=ON")
endif()
if(ASICEN_BUILD_PCSC_IFD AND UNIX AND NOT ANDROID)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ASICEN_PCSC_LITE QUIET libpcsclite)
    endif()
    find_path(ASICEN_PCSC_IFD_INCLUDE_DIR
        NAMES ifdhandler.h
        HINTS ${ASICEN_PCSC_LITE_INCLUDE_DIRS}
        PATH_SUFFIXES PCSC PCSC.framework/Headers
    )
    if(ASICEN_PCSC_IFD_INCLUDE_DIR)
        set(ASICEN_PCSC_IFD_AVAILABLE ON)
        set_target_properties(asicen_portable_core PROPERTIES
            POSITION_INDEPENDENT_CODE ON
        )
        add_library(asicen_pcsc_ifd_core STATIC
            ${ASICEN_PX4_ROOT}/src/pcsc_ifd_adapter.cpp
        )
        target_include_directories(asicen_pcsc_ifd_core
            PUBLIC ${ASICEN_PX4_ROOT}/include
        )
        target_compile_definitions(asicen_pcsc_ifd_core PRIVATE
            ASICEN_PCSC_DEVICE_PREFIX="asicen-userland:" ASICEN_PROFILE_ASICEN=1)
        target_compile_features(asicen_pcsc_ifd_core PUBLIC cxx_std_17)
        target_link_libraries(asicen_pcsc_ifd_core PUBLIC asicen_portable_core)
        set_target_properties(asicen_pcsc_ifd_core PROPERTIES
            POSITION_INDEPENDENT_CODE ON
        )
        asicen_enable_project_warnings(asicen_pcsc_ifd_core)

        add_library(asicen_ifdhandler SHARED ${ASICEN_PX4_ROOT}/src/pcsc_ifd.cpp)
        target_include_directories(asicen_ifdhandler PRIVATE
            ${ASICEN_PCSC_IFD_INCLUDE_DIR}
            ${ASICEN_PX4_ROOT}/include
        )
        target_compile_features(asicen_ifdhandler PRIVATE cxx_std_17)
        target_link_libraries(asicen_ifdhandler PRIVATE
            asicen_pcsc_ifd_core
        )
        set_target_properties(asicen_ifdhandler PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES
            OUTPUT_NAME asicen-userland-ifd
        )
        if(APPLE)
            set(ASICEN_IFD_BUNDLE_DIR
                "${CMAKE_CURRENT_BINARY_DIR}/asicen-userland-ifd.bundle")
            set(ASICEN_IFD_BUNDLE_EXECUTABLE_NAME
                "${CMAKE_SHARED_LIBRARY_PREFIX}asicen-userland-ifd${CMAKE_SHARED_LIBRARY_SUFFIX}")
            set(ASICEN_IFD_BUNDLE_INFO_PLIST
                "${CMAKE_CURRENT_BINARY_DIR}/asicen-userland-ifd.Info.plist")
            configure_file(
                packaging/pcsc/macos/Info.plist.in
                "${ASICEN_IFD_BUNDLE_INFO_PLIST}"
                @ONLY
            )
            add_custom_command(
                OUTPUT
                "${ASICEN_IFD_BUNDLE_DIR}/Contents/Info.plist"
                "${ASICEN_IFD_BUNDLE_DIR}/Contents/MacOS/${ASICEN_IFD_BUNDLE_EXECUTABLE_NAME}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                "${ASICEN_IFD_BUNDLE_DIR}/Contents/MacOS"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ASICEN_IFD_BUNDLE_INFO_PLIST}"
                "${ASICEN_IFD_BUNDLE_DIR}/Contents/Info.plist"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:asicen_ifdhandler>"
                "${ASICEN_IFD_BUNDLE_DIR}/Contents/MacOS/${ASICEN_IFD_BUNDLE_EXECUTABLE_NAME}"
                DEPENDS asicen_ifdhandler "${ASICEN_IFD_BUNDLE_INFO_PLIST}"
                COMMENT "Creating asicen-userland macOS IFD bundle"
                VERBATIM
            )
            add_custom_target(asicen_ifd_bundle ALL
                DEPENDS
                "${ASICEN_IFD_BUNDLE_DIR}/Contents/Info.plist"
                "${ASICEN_IFD_BUNDLE_DIR}/Contents/MacOS/${ASICEN_IFD_BUNDLE_EXECUTABLE_NAME}"
            )
            set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES
                "${ASICEN_IFD_BUNDLE_DIR}")
            target_link_options(asicen_ifdhandler PRIVATE
                "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/scripts/asicen-ifd.exports.macos"
            )
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_link_options(asicen_ifdhandler PRIVATE
                "LINKER:--as-needed"
                "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/scripts/asicen-ifd.exports.map"
            )
        endif()
        asicen_enable_project_warnings(asicen_ifdhandler)
    elseif(ASICEN_REQUIRE_PCSC_IFD)
        message(FATAL_ERROR
            "PC/SC IFD header ifdhandler.h was not found; install libpcsclite-dev or provide ASICEN_PCSC_IFD_INCLUDE_DIR")
    else()
        message(STATUS "PC/SC IFD header ifdhandler.h not found; asicen_ifdhandler is disabled")
    endif()
elseif(ASICEN_REQUIRE_PCSC_IFD)
    message(FATAL_ERROR "The PC/SC IFD Handler requires a non-Android POSIX host")
endif()

if(TARGET asicen_ifdhandler)
    if(ASICEN_BUILD_TESTS)
        add_executable(asicen-ifd-prefix-tests userland/tests/asicen_ifd_prefix_tests.cpp)
        target_link_libraries(asicen-ifd-prefix-tests PRIVATE asicen_pcsc_ifd_core)
        asicen_enable_project_warnings(asicen-ifd-prefix-tests)
    endif()
endif()

if(UNIX AND ASICEN_ENABLE_LIBUSB AND (ASICEN_BUILD_TOOLS OR ASICEN_BUILD_TESTS))
    add_library(asicen_diagnostic_support STATIC userland/src/diagnostic_probe.cpp)
    target_include_directories(asicen_diagnostic_support PUBLIC userland/include)
    target_link_libraries(asicen_diagnostic_support PUBLIC asicen_libusb)
    asicen_enable_project_warnings(asicen_diagnostic_support)

    foreach(probe ts frontend card)
        add_executable(asicen-${probe}-probe userland/tools/${probe}_probe.cpp)
        target_link_libraries(asicen-${probe}-probe PRIVATE asicen_diagnostic_support)
        asicen_enable_project_warnings(asicen-${probe}-probe)
    endforeach()

    add_executable(asicen-probe userland/tools/asicen_probe.cpp)
    target_link_libraries(asicen-probe PRIVATE asicen_diagnostic_support)
    asicen_enable_project_warnings(asicen-probe)

    add_executable(asicen-frontend userland/tools/asicen_frontend.cpp)
    target_link_libraries(asicen-frontend PRIVATE asicen_diagnostic_support)
    asicen_enable_project_warnings(asicen-frontend)
endif()

if(UNIX AND (ASICEN_BUILD_TOOLS OR ASICEN_BUILD_TESTS))
    add_executable(asicen-transform userland/tools/asicen_transform.cpp)
    target_link_libraries(asicen-transform PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-transform)

endif()

if(UNIX OR WIN32)
    add_executable(asicend
        userland/tools/asicend.cpp
        ${ASICEN_PX4_ROOT}/tools/px4d_signals.cpp)
    target_link_libraries(asicend PRIVATE asicen_core asicen_portable_core Threads::Threads)
    target_include_directories(asicend PRIVATE ${ASICEN_PX4_ROOT}/tools)
    if(WIN32)
        target_sources(asicend PRIVATE ${ASICEN_PX4_ROOT}/tools/px4d_windows_stdin.cpp)
        target_link_libraries(asicend PRIVATE shell32)
    endif()
    asicen_enable_project_warnings(asicend)

    if(ASICEN_ENABLE_LIBUSB)
        target_sources(asicend PRIVATE userland/tools/asicend_hardware.cpp)
        target_compile_definitions(asicend PRIVATE ASICEN_ENABLE_LIBUSB=1)
        target_link_libraries(asicend PRIVATE asicen_libusb)
    endif()

    if(UNIX AND (ASICEN_BUILD_TOOLS OR ASICEN_BUILD_TESTS))
        # asicend-mock: offline mock service, separate from the real
        # hardware daemon. Not part of the distributed product.
        add_executable(asicend-mock
            userland/tools/asicend_mock.cpp
            ${ASICEN_PX4_ROOT}/tools/px4d_signals.cpp)
        target_include_directories(asicend-mock PRIVATE ${ASICEN_PX4_ROOT}/tools)
        target_link_libraries(asicend-mock PRIVATE asicen_px4_mock asicen_core Threads::Threads)
        asicen_enable_project_warnings(asicend-mock)

    endif()

    add_executable(asicenctl
        userland/tools/asicenctl.cpp
        ${ASICEN_PX4_ROOT}/tools/px4ctl_format.cpp)
    target_include_directories(asicenctl PRIVATE
        ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/tools)
    target_compile_definitions(asicenctl PRIVATE ASICEN_PRODUCT_CLI=1)
    target_link_libraries(asicenctl PRIVATE asicen_portable_core)
    asicen_enable_project_warnings(asicenctl)

    if(WIN32)
        add_executable(asicen-ts
            userland/tools/asicen_ts_windows.cpp
            ${ASICEN_PX4_ROOT}/tools/px4_ts_windows_output.cpp
            ${ASICEN_PX4_ROOT}/tools/px4_ts_core.cpp)
        target_link_options(asicen-ts PRIVATE "LINKER:--stack,8388608")
        target_link_libraries(asicen-ts PRIVATE shell32)
        target_link_libraries(asicenctl PRIVATE shell32)
    else()
        add_executable(asicen-ts
            userland/tools/asicen_ts.cpp
            ${ASICEN_PX4_ROOT}/tools/px4_ts_core.cpp
            ${ASICEN_PX4_ROOT}/tools/px4_ts_posix.cpp)
    endif()
    target_include_directories(asicen-ts PRIVATE
        ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/tools)
    target_compile_definitions(asicen-ts PRIVATE ASICEN_PRODUCT_CLI=1)
    target_link_libraries(asicen-ts PRIVATE asicen_core asicen_portable_core)
    asicen_enable_project_warnings(asicen-ts)
endif()

if(ASICEN_BUILD_TESTS AND ASICEN_ENABLE_LIBUSB)
    add_executable(asicen-libusb-acquisition-tests
        userland/tests/libusb_acquisition_tests.cpp)
    target_link_libraries(asicen-libusb-acquisition-tests PRIVATE asicen_libusb)
    asicen_enable_project_warnings(asicen-libusb-acquisition-tests)
    add_test(NAME asicen-libusb-acquisition-tests COMMAND asicen-libusb-acquisition-tests)
endif()

if(ASICEN_BUILD_TESTS AND NOT WIN32)
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
        ${ASICEN_PX4_ROOT}/tests/ipc_tests.cpp
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
    asicen_enable_project_warnings(px4-portable-tests)
    add_test(NAME px4-portable-tests COMMAND px4-portable-tests)

    if(UNIX)
        if(ASICEN_PCSC_IFD_AVAILABLE)
            add_executable(px4-pcsc-ifd-tests
                third_party/px4-userland/userland/src/pcsc_ifd_adapter.cpp
                third_party/px4-userland/userland/src/pcsc_ifd.cpp
                third_party/px4-userland/userland/tests/pcsc_ifd_tests.cpp)
            target_include_directories(px4-pcsc-ifd-tests PRIVATE
                third_party/px4-userland/userland/include
                third_party/px4-userland/userland/tests
                ${ASICEN_PCSC_IFD_INCLUDE_DIR})
            target_link_libraries(px4-pcsc-ifd-tests PRIVATE
                px4_portable_reference Threads::Threads)
            asicen_enable_project_warnings(px4-pcsc-ifd-tests)
            add_test(NAME px4-pcsc-ifd-tests COMMAND px4-pcsc-ifd-tests)
            set_tests_properties(px4-pcsc-ifd-tests PROPERTIES TIMEOUT 20)
            if(TARGET asicen-ifd-prefix-tests)
                add_test(NAME asicen-ifd-prefix-tests COMMAND asicen-ifd-prefix-tests)
            endif()
            if(ASICEN_ENABLE_LIBUSB)
                add_executable(asicen-card-only-service-tests
                    userland/tests/card_only_service_tests.cpp)
                target_include_directories(asicen-card-only-service-tests PRIVATE
                    third_party/px4-userland/userland/include
                    third_party/px4-userland/userland/tests)
                target_link_libraries(asicen-card-only-service-tests PRIVATE
                    asicen_libusb asicen_pcsc_ifd_core)
                asicen_enable_project_warnings(asicen-card-only-service-tests)
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
    asicen_enable_project_warnings(asicen-product-profile-tests)
    add_test(NAME asicen-product-profile-tests COMMAND asicen-product-profile-tests)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME asicen-model-cli-tests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/userland/tests/model_cli_tests.py $<TARGET_FILE_DIR:asicend>)
    if(ASICEN_ENABLE_LIBUSB)
        add_test(NAME asicen-fd-cli-tests
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/userland/tests/fd_cli_tests.py $<TARGET_FILE_DIR:asicend>)
    endif()
    add_executable(asicen-legacy-frontend-profile-tests userland/tests/legacy_frontend_profile_tests.cpp)
    target_link_libraries(asicen-legacy-frontend-profile-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-legacy-frontend-profile-tests)
    add_test(NAME asicen-legacy-frontend-profile-tests COMMAND asicen-legacy-frontend-profile-tests)

    foreach(v2_test frontend nmi)
        add_executable(asicen-v2-${v2_test}-tests userland/tests/v2_${v2_test}_tests.cpp)
        target_link_libraries(asicen-v2-${v2_test}-tests PRIVATE asicen_core)
        asicen_enable_project_warnings(asicen-v2-${v2_test}-tests)
        add_test(NAME asicen-v2-${v2_test}-tests COMMAND asicen-v2-${v2_test}-tests)
    endforeach()

    add_executable(asicen-model-ipc-tests userland/tests/model_ipc_tests.cpp)
    target_link_libraries(asicen-model-ipc-tests PRIVATE asicen_core asicen_px4_mock)
    asicen_enable_project_warnings(asicen-model-ipc-tests)
    add_test(NAME asicen-model-ipc-tests COMMAND asicen-model-ipc-tests)

    add_executable(asicen-product-lnb-tests
        userland/tests/product_lnb_tests.cpp
        ${ASICEN_PX4_ROOT}/tools/px4_ts_core.cpp)
    target_include_directories(asicen-product-lnb-tests PRIVATE ${ASICEN_PX4_ROOT}/tools)
    target_compile_definitions(asicen-product-lnb-tests PRIVATE ASICEN_PRODUCT_CLI=1)
    target_link_libraries(asicen-product-lnb-tests PRIVATE asicen_core asicen_px4_mock)
    asicen_enable_project_warnings(asicen-product-lnb-tests)
    add_test(NAME asicen-product-lnb-tests COMMAND asicen-product-lnb-tests)

    add_executable(asicen-hardware-ownership-tests
        userland/tests/hardware_ownership_tests.cpp
        userland/src/hardware_ownership.cpp
        userland/src/device_profile.cpp)
    target_include_directories(asicen-hardware-ownership-tests PRIVATE userland/include)
    asicen_enable_project_warnings(asicen-hardware-ownership-tests)
    add_test(NAME asicen-hardware-ownership-tests COMMAND asicen-hardware-ownership-tests)

    if(ASICEN_ENABLE_LIBUSB)
        add_executable(asicen-libusb-capture-lifecycle-tests
            userland/tests/libusb_capture_lifecycle_tests.cpp)
        target_link_libraries(asicen-libusb-capture-lifecycle-tests PRIVATE asicen_libusb)
        asicen_enable_project_warnings(asicen-libusb-capture-lifecycle-tests)
        add_test(NAME asicen-libusb-capture-lifecycle-tests
            COMMAND asicen-libusb-capture-lifecycle-tests)
        # All model lifecycles include real board/tuner delays: roughly 20 seconds
        # together on macOS CI. This suite limit leaves scheduling headroom without
        # changing any backend operation's deadline.
        set_tests_properties(asicen-libusb-capture-lifecycle-tests PROPERTIES TIMEOUT 60)

        add_executable(asicen-libusb-lnb-lifecycle-tests userland/tests/libusb_lnb_lifecycle_tests.cpp)
        target_link_libraries(asicen-libusb-lnb-lifecycle-tests PRIVATE asicen_libusb)
        asicen_enable_project_warnings(asicen-libusb-lnb-lifecycle-tests)
        add_test(NAME asicen-libusb-lnb-lifecycle-tests COMMAND asicen-libusb-lnb-lifecycle-tests)
        set_tests_properties(asicen-libusb-lnb-lifecycle-tests PROPERTIES TIMEOUT 15)

        add_executable(asicen-libusb-fd-tests
            userland/tests/libusb_fd_tests.cpp)
        target_link_libraries(asicen-libusb-fd-tests PRIVATE asicen_libusb)
        asicen_enable_project_warnings(asicen-libusb-fd-tests)
        add_test(NAME asicen-libusb-fd-tests COMMAND asicen-libusb-fd-tests)
    endif()

    add_executable(asicen-enclosure-lock-tests
        userland/tests/enclosure_lock_tests.cpp)
    target_include_directories(asicen-enclosure-lock-tests PRIVATE userland/include)
    target_link_libraries(asicen-enclosure-lock-tests PRIVATE asicen_portable_core)
    asicen_enable_project_warnings(asicen-enclosure-lock-tests)
    add_test(NAME asicen-enclosure-lock-tests COMMAND asicen-enclosure-lock-tests)

    add_executable(asicen-hardware-session-tests
        userland/tests/hardware_stream_session_tests.cpp)
    target_include_directories(asicen-hardware-session-tests PRIVATE
        userland/include ${ASICEN_PX4_ROOT}/include)
    target_link_libraries(asicen-hardware-session-tests PRIVATE asicen_hardware_session asicen_px4_mock)
    asicen_enable_project_warnings(asicen-hardware-session-tests)
    add_test(NAME asicen-hardware-session-tests COMMAND asicen-hardware-session-tests)
    set_tests_properties(asicen-hardware-session-tests PROPERTIES TIMEOUT 10)

    add_executable(asicen-hardware-ipc-integration-tests
        userland/tests/hardware_ipc_integration_tests.cpp)
    target_include_directories(asicen-hardware-ipc-integration-tests PRIVATE
        userland/include ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/tests)
    target_link_libraries(asicen-hardware-ipc-integration-tests PRIVATE asicen_hardware_session asicen_px4_mock)
    asicen_enable_project_warnings(asicen-hardware-ipc-integration-tests)
    add_test(NAME asicen-hardware-ipc-integration-tests
        COMMAND asicen-hardware-ipc-integration-tests)
    set_tests_properties(asicen-hardware-ipc-integration-tests PROPERTIES TIMEOUT 10)

    add_executable(asicen-capture-drain-tests userland/tests/capture_drain_tests.cpp)
    target_include_directories(asicen-capture-drain-tests PRIVATE userland/include)
    target_link_libraries(asicen-capture-drain-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-capture-drain-tests)
    add_test(NAME asicen-capture-drain-tests COMMAND asicen-capture-drain-tests)
    set_tests_properties(asicen-capture-drain-tests PROPERTIES TIMEOUT 10)

    add_executable(asicen-secure-entropy-tests
        userland/tests/secure_entropy_tests.cpp
        ${ASICEN_PX4_ROOT}/tests/posix_tuner_nonce_tests.cpp)
    target_include_directories(asicen-secure-entropy-tests PRIVATE ${ASICEN_PX4_ROOT}/src)
    target_link_libraries(asicen-secure-entropy-tests PRIVATE asicen_portable_core)
    asicen_enable_project_warnings(asicen-secure-entropy-tests)
    add_test(NAME asicen-secure-entropy-tests COMMAND asicen-secure-entropy-tests)

    add_executable(asicen-hardware-gpio-guard-tests
        userland/tests/hardware_gpio_guard_tests.cpp)
    target_include_directories(asicen-hardware-gpio-guard-tests PRIVATE userland/include)
    target_link_libraries(asicen-hardware-gpio-guard-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-hardware-gpio-guard-tests)
    add_test(NAME asicen-hardware-gpio-guard-tests COMMAND asicen-hardware-gpio-guard-tests)

    add_executable(asicen-gpio-snapshot-tests
        userland/tests/gpio_snapshot_tests.cpp)
    target_include_directories(asicen-gpio-snapshot-tests PRIVATE userland/include)
    asicen_enable_project_warnings(asicen-gpio-snapshot-tests)
    add_test(NAME asicen-gpio-snapshot-tests COMMAND asicen-gpio-snapshot-tests)

    add_executable(asicen-powered-init-sequence-tests
        userland/tests/powered_init_sequence_tests.cpp)
    target_include_directories(asicen-powered-init-sequence-tests PRIVATE userland/include)
    asicen_enable_project_warnings(asicen-powered-init-sequence-tests)
    add_test(NAME asicen-powered-init-sequence-tests
        COMMAND asicen-powered-init-sequence-tests)

    add_executable(asicen-product-shutdown-tests
        userland/tests/product_shutdown_hooks_tests.cpp)
    target_include_directories(asicen-product-shutdown-tests PRIVATE
        userland/include ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/tests)
    target_link_libraries(asicen-product-shutdown-tests PRIVATE asicen_px4_mock)
    asicen_enable_project_warnings(asicen-product-shutdown-tests)
    add_test(NAME asicen-product-shutdown-tests COMMAND asicen-product-shutdown-tests)
    set_tests_properties(asicen-product-shutdown-tests PROPERTIES TIMEOUT 10)

    add_executable(asicen-product-worker-routing-tests
        userland/tests/product_worker_routing_tests.cpp)
    target_include_directories(asicen-product-worker-routing-tests PRIVATE
        userland/include ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/tests)
    target_link_libraries(asicen-product-worker-routing-tests PRIVATE asicen_px4_mock)
    asicen_enable_project_warnings(asicen-product-worker-routing-tests)
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
            $<TARGET_FILE:asicend-mock> $<TARGET_FILE:asicenctl> $<TARGET_FILE:asicen-ts>)
        set_tests_properties(asicen-product-cli-integration PROPERTIES TIMEOUT 15)
    endif()

    add_executable(asicen-protocol-tests userland/tests/protocol_tests.cpp)
    target_link_libraries(asicen-protocol-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-protocol-tests)
    add_test(NAME asicen-protocol-tests COMMAND asicen-protocol-tests)

    add_executable(asicen-write-protocol-tests userland/tests/write_protocol_tests.cpp)
    target_link_libraries(asicen-write-protocol-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-write-protocol-tests)
    add_test(NAME asicen-write-protocol-tests COMMAND asicen-write-protocol-tests)

    add_executable(asicen-enclosure-routing-tests userland/tests/enclosure_routing_tests.cpp)
    target_link_libraries(asicen-enclosure-routing-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-enclosure-routing-tests)
    add_test(NAME asicen-enclosure-routing-tests COMMAND asicen-enclosure-routing-tests)

    add_executable(asicen-enclosure-grouping-tests userland/tests/enclosure_grouping_tests.cpp)
    target_link_libraries(asicen-enclosure-grouping-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-enclosure-grouping-tests)
    add_test(NAME asicen-enclosure-grouping-tests COMMAND asicen-enclosure-grouping-tests)

    add_executable(asicen-ts-framer-tests userland/tests/ts_framer_tests.cpp)
    target_link_libraries(asicen-ts-framer-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-ts-framer-tests)
    add_test(NAME asicen-ts-framer-tests COMMAND asicen-ts-framer-tests)

    add_executable(asicen-card-mailbox-tests userland/tests/card_mailbox_tests.cpp)
    target_link_libraries(asicen-card-mailbox-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-card-mailbox-tests)
    add_test(NAME asicen-card-mailbox-tests COMMAND asicen-card-mailbox-tests)

    add_executable(asicen-card-mailbox-hardware-tests
        userland/tests/card_mailbox_hardware_tests.cpp
        userland/src/card_mailbox_hardware.cpp)
    target_include_directories(asicen-card-mailbox-hardware-tests PRIVATE
        userland/include ${ASICEN_PX4_ROOT}/include)
    target_link_libraries(asicen-card-mailbox-hardware-tests PRIVATE
        asicen_core asicen_px4_mock Threads::Threads)
    asicen_enable_project_warnings(asicen-card-mailbox-hardware-tests)
    add_test(NAME asicen-card-mailbox-hardware-tests
        COMMAND asicen-card-mailbox-hardware-tests)
    set_tests_properties(asicen-card-mailbox-hardware-tests PROPERTIES TIMEOUT 15)

    add_executable(asicen-loader-firmware-tests userland/tests/loader_firmware_tests.cpp)
    target_link_libraries(asicen-loader-firmware-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-loader-firmware-tests)
    add_test(NAME asicen-loader-firmware-tests COMMAND asicen-loader-firmware-tests)

    add_executable(asicen-frontend-sequence-tests userland/tests/frontend_sequence_tests.cpp)
    target_link_libraries(asicen-frontend-sequence-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-frontend-sequence-tests)
    add_test(NAME asicen-frontend-sequence-tests COMMAND asicen-frontend-sequence-tests)

    add_executable(asicen-shared-demod-tests userland/tests/shared_demod_tests.cpp)
    target_link_libraries(asicen-shared-demod-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-shared-demod-tests)
    add_test(NAME asicen-shared-demod-tests COMMAND asicen-shared-demod-tests)

    add_executable(asicen-frontend-tune-tests userland/tests/frontend_tune_tests.cpp)
    target_link_libraries(asicen-frontend-tune-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-frontend-tune-tests)
    add_test(NAME asicen-frontend-tune-tests COMMAND asicen-frontend-tune-tests)

    add_executable(asicen-frontend-gain-tests userland/tests/frontend_gain_tests.cpp)
    target_link_libraries(asicen-frontend-gain-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-frontend-gain-tests)
    add_test(NAME asicen-frontend-gain-tests COMMAND asicen-frontend-gain-tests)

    add_executable(asicen-satellite-tune-tests userland/tests/satellite_tune_tests.cpp)
    target_link_libraries(asicen-satellite-tune-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-satellite-tune-tests)
    add_test(NAME asicen-satellite-tune-tests COMMAND asicen-satellite-tune-tests)

    add_executable(asicen-stream-capture-tests userland/tests/stream_capture_tests.cpp)
    target_link_libraries(asicen-stream-capture-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-stream-capture-tests)
    add_test(NAME asicen-stream-capture-tests COMMAND asicen-stream-capture-tests)

    add_executable(asicen-queued-capture-tests userland/tests/queued_capture_tests.cpp)
    target_link_libraries(asicen-queued-capture-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-queued-capture-tests)
    add_test(NAME asicen-queued-capture-tests COMMAND asicen-queued-capture-tests)

    add_executable(asicen-stream-setup-tests userland/tests/stream_setup_tests.cpp)
    target_link_libraries(asicen-stream-setup-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-stream-setup-tests)
    add_test(NAME asicen-stream-setup-tests COMMAND asicen-stream-setup-tests)

    add_executable(asicen-cf-chunk-address-tests userland/tests/cf_chunk_address_tests.cpp)
    target_link_libraries(asicen-cf-chunk-address-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-cf-chunk-address-tests)
    add_test(NAME asicen-cf-chunk-address-tests COMMAND asicen-cf-chunk-address-tests)

    add_executable(asicen-capture-stdout-tests userland/tests/capture_stdout_tests.cpp)
    target_link_libraries(asicen-capture-stdout-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-capture-stdout-tests)
    add_test(NAME asicen-capture-stdout-tests COMMAND asicen-capture-stdout-tests)

    add_executable(asicen-transport-transform-tests userland/tests/transport_transform_tests.cpp)
    target_link_libraries(asicen-transport-transform-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-transport-transform-tests)
    add_test(NAME asicen-transport-transform-tests COMMAND asicen-transport-transform-tests)

    add_executable(asicen-link-seed-diagnostic-tests userland/tests/link_seed_diagnostic_tests.cpp)
    target_link_libraries(asicen-link-seed-diagnostic-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-link-seed-diagnostic-tests)
    add_test(NAME asicen-link-seed-diagnostic-tests COMMAND asicen-link-seed-diagnostic-tests)

    add_executable(asicen-transport-capture-tests userland/tests/transport_capture_tests.cpp)
    target_link_libraries(asicen-transport-capture-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-transport-capture-tests)
    add_test(NAME asicen-transport-capture-tests COMMAND asicen-transport-capture-tests)

    if(UNIX)
        add_test(
            NAME asicen-mock-integration
            COMMAND bash
            ${CMAKE_CURRENT_SOURCE_DIR}/userland/tests/mock_integration.sh
            $<TARGET_FILE:asicend-mock>
            $<TARGET_FILE:asicen-ts>
        )

        # IFD bundle proxy smoke: exercises the packaged IFD plugin against the
        # mock daemon over the IPC proxy. Requires both the IFD handler target
        # (built when PC/SC headers are available) and asicend-mock.
        if(TARGET asicen_ifdhandler AND TARGET asicend-mock)
            add_test(
                NAME asicen-ifd-bundle-smoke
                COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/macos-ifd-bundle-smoke.py
                $<TARGET_FILE:asicen_ifdhandler>
                $<TARGET_FILE:asicend-mock>
            )
            set_tests_properties(asicen-ifd-bundle-smoke PROPERTIES TIMEOUT 30)
        endif()
    endif()
endif()

if(WIN32)
    if(TARGET px4_portable_reference)
        target_link_libraries(px4_portable_reference PUBLIC advapi32 bcrypt ws2_32)
    endif()
    target_link_libraries(asicen_portable_core PUBLIC advapi32 bcrypt ws2_32)
    target_link_libraries(asicen_core PUBLIC bcrypt)
endif()

if(ASICEN_BUILD_TESTS AND TARGET asicen_ifdhandler)
    add_test(NAME asicen_ifd_symbols
        COMMAND ${CMAKE_COMMAND}
        -DNM=${CMAKE_NM}
        -DLIBRARY=$<TARGET_FILE:asicen_ifdhandler>
        -DIS_APPLE=${APPLE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-ifd-symbols.cmake)
endif()
if(ASICEN_ENABLE_LIBUSB AND (WIN32 OR ASICEN_BUILD_TESTS))
    target_sources(asicen_libusb PRIVATE userland/src/windows/libusb_stream_policy.cpp)
    if(ASICEN_BUILD_TESTS)
        enable_testing()
        add_executable(asicen_windows_libusb_stream_policy_tests
            userland/tests/windows_libusb_stream_policy_tests.cpp)
        target_include_directories(asicen_windows_libusb_stream_policy_tests PRIVATE userland/src)
        target_link_libraries(asicen_windows_libusb_stream_policy_tests PRIVATE asicen_libusb)
        asicen_enable_project_warnings(asicen_windows_libusb_stream_policy_tests)
        add_test(NAME asicen_windows_libusb_stream_policy_tests
            COMMAND asicen_windows_libusb_stream_policy_tests)
    endif()
endif()

if(ASICEN_BUILD_TESTS)
    enable_testing()
    # Preserve the reference's portable helper API and FIFO oracle without
    # including synthetic transports in production builds.
    add_library(asicen_transport_mock STATIC ${ASICEN_PX4_ROOT}/src/mock_transport.cpp)
    target_link_libraries(asicen_transport_mock PUBLIC asicen_portable_core)
    asicen_enable_project_warnings(asicen_transport_mock)

    add_executable(asicen-portable-helper-tests userland/tests/portable_helper_tests.cpp)
    target_link_libraries(asicen-portable-helper-tests PRIVATE asicen_transport_mock)
    asicen_enable_project_warnings(asicen-portable-helper-tests)
    add_test(NAME asicen-portable-helper-tests COMMAND asicen-portable-helper-tests)

    add_executable(asicen-capture-transport-tests userland/tests/capture_transport_tests.cpp)
    target_link_libraries(asicen-capture-transport-tests PRIVATE asicen_transport_mock asicen_core)
    asicen_enable_project_warnings(asicen-capture-transport-tests)
    add_test(NAME asicen-capture-transport-tests COMMAND asicen-capture-transport-tests)

    add_executable(asicen-product-ipc-codec-tests
        ${ASICEN_PX4_ROOT}/tests/asicen_ipc_tests_main.cpp
        ${ASICEN_PX4_ROOT}/tests/ipc_tests.cpp)
    target_compile_definitions(asicen-product-ipc-codec-tests PRIVATE ASICEN_PROFILE_ASICEN=1)
    target_include_directories(asicen-product-ipc-codec-tests PRIVATE ${ASICEN_PX4_ROOT}/tests)
    target_link_libraries(asicen-product-ipc-codec-tests PRIVATE asicen_px4_mock)
    asicen_enable_project_warnings(asicen-product-ipc-codec-tests)
    add_test(NAME asicen-product-ipc-codec-tests COMMAND asicen-product-ipc-codec-tests)
endif()

if(ASICEN_BUILD_TESTS AND WIN32)
    add_executable(asicen_windows_tests
        ${ASICEN_PX4_ROOT}/tests/windows_imported_tests_main.cpp
        ${ASICEN_PX4_ROOT}/tests/card_tests.cpp
        ${ASICEN_PX4_ROOT}/tests/card_service_tests.cpp
        ${ASICEN_PX4_ROOT}/tests/ipc_tests.cpp
        ${ASICEN_PX4_ROOT}/tests/ipc_state_tests.cpp
        ${ASICEN_PX4_ROOT}/tests/tuner_service_tests.cpp
        ${ASICEN_PX4_ROOT}/src/card.cpp)
    target_link_libraries(asicen_windows_tests PRIVATE px4_portable_reference)
    add_executable(asicen_windows_platform_tests
        ${ASICEN_PX4_ROOT}/tests/windows_platform_tests.cpp
        userland/src/loader_firmware.cpp userland/src/firmware.cpp userland/src/device_profile.cpp)
    target_compile_definitions(asicen_windows_platform_tests PRIVATE ASICEN_NATIVE_LOADER_TEST=1)
    target_include_directories(asicen_windows_platform_tests PRIVATE userland/include)
    target_link_libraries(asicen_windows_platform_tests PRIVATE px4_portable_reference)
    add_executable(asicen_windows_workers_tests
        ${ASICEN_PX4_ROOT}/tests/windows_worker_tests_main.cpp
        ${ASICEN_PX4_ROOT}/tests/control_workers_tests.cpp
        ${ASICEN_PX4_ROOT}/src/control_workers.cpp)
    target_compile_definitions(asicen_windows_workers_tests PRIVATE PX4_CONTROL_WORKERS_TEST_ACCESS=1)
    target_link_libraries(asicen_windows_workers_tests PRIVATE px4_portable_reference)
    add_executable(asicen_windows_ts_output_tests
        ${ASICEN_PX4_ROOT}/tests/windows_ts_output_tests.cpp
        ${ASICEN_PX4_ROOT}/tools/px4_ts_windows_output.cpp
        ${ASICEN_PX4_ROOT}/tools/px4_ts_core.cpp)
    target_link_libraries(asicen_windows_ts_output_tests PRIVATE px4_portable_reference)
    add_executable(asicen_windows_control_tests ${ASICEN_PX4_ROOT}/tests/windows_control_tests.cpp)
    target_link_libraries(asicen_windows_control_tests PRIVATE px4_portable_reference)
    add_executable(asicen_windows_stdin_tests
        ${ASICEN_PX4_ROOT}/tests/windows_stdin_monitor_tests.cpp
        ${ASICEN_PX4_ROOT}/tools/px4d_windows_stdin.cpp)
    foreach(target asicen_windows_tests asicen_windows_platform_tests asicen_windows_workers_tests
        asicen_windows_ts_output_tests asicen_windows_control_tests asicen_windows_stdin_tests)
        target_include_directories(${target} PRIVATE
            ${ASICEN_PX4_ROOT}/include ${ASICEN_PX4_ROOT}/src
            ${ASICEN_PX4_ROOT}/tools ${ASICEN_PX4_ROOT}/tests)
        asicen_enable_project_warnings(${target})
        add_test(NAME ${target} COMMAND ${target})
    endforeach()
endif()

if(ASICEN_BUILD_ANDROID_LINKCHECK)
    if(NOT ANDROID)
        message(FATAL_ERROR "ASICEN_BUILD_ANDROID_LINKCHECK requires an Android toolchain")
    endif()
    if(NOT ASICEN_ENABLE_LIBUSB)
        message(FATAL_ERROR "ASICEN_BUILD_ANDROID_LINKCHECK requires ASICEN_ENABLE_LIBUSB=ON")
    endif()
    add_executable(asicen-android-linkcheck userland/tools/android_linkcheck.cpp)
    target_link_libraries(asicen-android-linkcheck PRIVATE asicen_libusb)
    target_link_libraries(asicen-android-linkcheck PRIVATE log c)
    target_include_directories(asicen-android-linkcheck PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/userland/include)
    target_compile_features(asicen-android-linkcheck PRIVATE cxx_std_17)
    asicen_enable_project_warnings(asicen-android-linkcheck)
    target_link_options(asicen-android-linkcheck PRIVATE
        -Wl,--no-as-needed
        -Wl,--no-gc-sections
        -Wl,--undefined=libusb_init
        -Wl,--undefined=libusb_wrap_sys_device
        -Wl,--undefined=libusb_close
        -Wl,--undefined=__android_log_print
        -Wl,--undefined=memcpy
    )
endif()

if(ANDROID AND ASICEN_ANDROID_LINK_MAP_DIR)
    file(MAKE_DIRECTORY "${ASICEN_ANDROID_LINK_MAP_DIR}")
    foreach(target asicend asicen-ts asicenctl)
        if(TARGET ${target})
            target_link_options(${target} PRIVATE
                "LINKER:-Map,${ASICEN_ANDROID_LINK_MAP_DIR}/${target}.map")
        endif()
    endforeach()
endif()

if(ASICEN_RELEASE_ASICEND_NO_BUILD_ID AND TARGET asicend)
    target_link_options(asicend PRIVATE "LINKER:--build-id=none")
endif()

if(ASICEN_STATIC_LINK_MAP_DIR)
    file(MAKE_DIRECTORY "${ASICEN_STATIC_LINK_MAP_DIR}")
    foreach(target asicend asicen-ts asicenctl)
        if(TARGET ${target})
            target_link_options(${target} PRIVATE
                "LINKER:-Map,${ASICEN_STATIC_LINK_MAP_DIR}/${target}.map")
        endif()
    endforeach()
endif()

if(ASICEN_BUILD_TESTS)
    add_executable(asicen-firmware-provider-tests
        ${ASICEN_PX4_ROOT}/tests/asicen_firmware_provider_tests.cpp)
    target_link_libraries(asicen-firmware-provider-tests PRIVATE asicen_core)
    asicen_enable_project_warnings(asicen-firmware-provider-tests)
    add_test(NAME asicen-firmware-provider-tests COMMAND asicen-firmware-provider-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/firmware/asicen-loader.bin)

    add_executable(asicen-tools-parity-tests userland/tests/tools_parity_tests.cpp)
    target_include_directories(asicen-tools-parity-tests PRIVATE userland/tools)
    target_link_libraries(asicen-tools-parity-tests PRIVATE asicen_core asicen_px4_mock)
    asicen_enable_project_warnings(asicen-tools-parity-tests)
    add_test(NAME asicen-tools-parity-tests COMMAND asicen-tools-parity-tests)

    add_executable(asicen-product-worker-boundaries-tests
        ${ASICEN_PX4_ROOT}/tests/windows_worker_tests_main.cpp
        ${ASICEN_PX4_ROOT}/tests/control_workers_tests.cpp
        ${ASICEN_PX4_ROOT}/src/control_workers.cpp)
    target_include_directories(asicen-product-worker-boundaries-tests PRIVATE
        ${ASICEN_PX4_ROOT}/src ${ASICEN_PX4_ROOT}/tests)
    target_compile_definitions(asicen-product-worker-boundaries-tests PRIVATE
        PX4_CONTROL_WORKERS_TEST_ACCESS=1 ASICEN_PROFILE_ASICEN=1)
    target_link_libraries(asicen-product-worker-boundaries-tests PRIVATE asicen_px4_mock)
    asicen_enable_project_warnings(asicen-product-worker-boundaries-tests)
    add_test(NAME asicen-product-worker-boundaries-tests COMMAND asicen-product-worker-boundaries-tests)

    add_executable(asicen-ifd-capability-tests
        ${ASICEN_PX4_ROOT}/tests/asicen_ifd_capability_tests.cpp
        ${ASICEN_PX4_ROOT}/src/pcsc_ifd_adapter.cpp)
    target_compile_definitions(asicen-ifd-capability-tests PRIVATE
        ASICEN_PROFILE_ASICEN=1 ASICEN_PCSC_DEVICE_PREFIX="asicen-userland:")
    target_link_libraries(asicen-ifd-capability-tests PRIVATE asicen_px4_mock)
    asicen_enable_project_warnings(asicen-ifd-capability-tests)
    add_test(NAME asicen-ifd-capability-tests COMMAND asicen-ifd-capability-tests)

    if(UNIX)
        add_executable(asicen-identity-endpoint-tests ${ASICEN_PX4_ROOT}/tests/identity_endpoint_tests.cpp)
        target_compile_definitions(asicen-identity-endpoint-tests PRIVATE ASICEN_PROFILE_ASICEN=1)
        target_include_directories(asicen-identity-endpoint-tests PRIVATE ${ASICEN_PX4_ROOT}/tests)
        target_link_libraries(asicen-identity-endpoint-tests PRIVATE asicen_px4_mock)
        asicen_enable_project_warnings(asicen-identity-endpoint-tests)
        add_test(NAME asicen-identity-endpoint-tests COMMAND asicen-identity-endpoint-tests)

    endif()
endif()

if(ASICEN_BUILD_TESTS AND NOT WIN32)
    if(TARGET asicen_diagnostic_support)
        foreach(test diagnostic_probe frontend_arguments probe_arguments)
            string(REPLACE "_" "-" target "asicen-${test}-tests")
            add_executable(${target} userland/tests/${test}_tests.cpp)
            target_link_libraries(${target} PRIVATE asicen_diagnostic_support)
            asicen_enable_project_warnings(${target})
            add_test(NAME ${target} COMMAND ${target})
        endforeach()
        add_test(NAME asicen-diagnostic-cli-tests
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/userland/tests/diagnostic_cli_tests.py
            $<TARGET_FILE:asicen-frontend> $<TARGET_FILE:asicen-probe> $<TARGET_FILE:asicen-transform>)
    endif()
endif()

if(ASICEN_BUILD_TESTS)
    add_executable(asicen-ts-argument-tests
        ${ASICEN_PX4_ROOT}/tests/asicen_ts_argument_tests.cpp
        ${ASICEN_PX4_ROOT}/tools/px4_ts_core.cpp)
    target_compile_definitions(asicen-ts-argument-tests PRIVATE ASICEN_PRODUCT_CLI=1)
    target_include_directories(asicen-ts-argument-tests PRIVATE ${ASICEN_PX4_ROOT}/tools)
    target_link_libraries(asicen-ts-argument-tests PRIVATE asicen_px4_mock)
    asicen_enable_project_warnings(asicen-ts-argument-tests)
    add_test(NAME asicen-ts-argument-tests COMMAND asicen-ts-argument-tests)
endif()

if(ASICEN_BUILD_TESTS)
    add_executable(asicen-tuner-settle-tests ${ASICEN_PX4_ROOT}/tests/tuner_service_settle_tests.cpp)
    target_link_libraries(asicen-tuner-settle-tests PRIVATE px4_portable_reference)
    asicen_enable_project_warnings(asicen-tuner-settle-tests)
    add_test(NAME asicen-tuner-settle-tests COMMAND asicen-tuner-settle-tests)
    if(UNIX)
        add_executable(asicen-signals-tests
            userland/tests/signals_tests_main.cpp
            ${ASICEN_PX4_ROOT}/tests/px4d_signal_tests.cpp
            ${ASICEN_PX4_ROOT}/tools/px4d_signals.cpp)
        target_include_directories(asicen-signals-tests PRIVATE ${ASICEN_PX4_ROOT}/tools)
        asicen_enable_project_warnings(asicen-signals-tests)
        add_test(NAME asicen-signals-tests COMMAND asicen-signals-tests)
    endif()
endif()
