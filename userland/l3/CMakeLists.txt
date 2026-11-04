# SPDX-License-Identifier: GPL-2.0-or-later
if(NOT ASICEN_L3_CANDIDATE_DIR)
    set(ASICEN_L3_CANDIDATE_DIR
        "${dtv_userland_core_SOURCE_DIR}/prototype/l3_owner" CACHE PATH
        "Core L3 owner candidate directory (include/dtv/l3_owner.h)")
endif()
if(NOT EXISTS "${ASICEN_L3_CANDIDATE_DIR}/include/dtv/l3_owner.h")
    message(FATAL_ERROR
        "ASICEN_BUILD_L3_ADAPTER needs the Core L3 candidate header at "
        "${ASICEN_L3_CANDIDATE_DIR}/include/dtv/l3_owner.h")
endif()

set(ASICEN_L3_CORE_CANDIDATE_SOURCES
    "${ASICEN_L3_CANDIDATE_DIR}/src/owner_engine.cpp"
    "${ASICEN_L3_CANDIDATE_DIR}/src/c_api.cpp")

add_library(asicen_l3 STATIC
    src/adapter.cpp
    src/factory.cpp
    ../src/device_profile.cpp
    ../src/enclosure.cpp
    ../src/frontend_facts.cpp
    ${ASICEN_L3_CORE_CANDIDATE_SOURCES})
target_include_directories(asicen_l3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ASICEN_L3_CANDIDATE_DIR}/include)
target_include_directories(asicen_l3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${ASICEN_PX4_ROOT}/include)
target_compile_features(asicen_l3 PUBLIC cxx_std_17)
target_compile_options(asicen_l3 PRIVATE -Wall -Wextra)
target_link_libraries(asicen_l3 PUBLIC Threads::Threads)
set_target_properties(asicen_l3 PROPERTIES CXX_EXTENSIONS OFF)

if(ASICEN_ENABLE_LIBUSB)
    add_library(asicen_l3_libusb STATIC src/libusb_host.cpp)
    target_link_libraries(asicen_l3_libusb PUBLIC asicen_l3 asicen_libusb)
    target_include_directories(asicen_l3_libusb PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(asicen_l3_libusb PUBLIC cxx_std_17)
    target_compile_options(asicen_l3_libusb PRIVATE -Wall -Wextra)
    set_target_properties(asicen_l3_libusb PROPERTIES CXX_EXTENSIONS OFF)
endif()

if(ASICEN_BUILD_TESTS)
    enable_language(C)

    add_executable(asicen-l3-c-contract-tests tests/l3_c_contract_tests.c)
    target_link_libraries(asicen-l3-c-contract-tests PRIVATE asicen_l3)
    target_include_directories(asicen-l3-c-contract-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ASICEN_L3_CANDIDATE_DIR}/include)
    target_compile_options(asicen-l3-c-contract-tests PRIVATE
        -Wall -Wextra -Werror)
    set_target_properties(asicen-l3-c-contract-tests PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
        LINKER_LANGUAGE CXX)
    add_test(NAME asicen-l3-c-contract-tests COMMAND asicen-l3-c-contract-tests)

    add_executable(asicen-l3-adapter-tests tests/l3_adapter_tests.cpp)
    target_link_libraries(asicen-l3-adapter-tests PRIVATE asicen_l3)
    target_include_directories(asicen-l3-adapter-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${ASICEN_L3_CANDIDATE_DIR}/include
        ${ASICEN_PX4_ROOT}/include)
    target_compile_options(asicen-l3-adapter-tests PRIVATE
        -Wall -Wextra -Werror)
    target_compile_features(asicen-l3-adapter-tests PRIVATE cxx_std_17)
    set_target_properties(asicen-l3-adapter-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME asicen-l3-adapter-tests COMMAND asicen-l3-adapter-tests)
endif()

if(ASICEN_L3_ENABLE_TSAN)
    foreach(target asicen_l3 asicen-l3-c-contract-tests asicen-l3-adapter-tests)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE
                -fsanitize=thread -fno-omit-frame-pointer -g)
            target_link_options(${target} PRIVATE -fsanitize=thread)
        endif()
    endforeach()
endif()

if(ASICEN_L3_ENABLE_ASAN)
    foreach(target asicen_l3 asicen-l3-c-contract-tests asicen-l3-adapter-tests)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE
                -fsanitize=address -fno-omit-frame-pointer -g)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    endforeach()
endif()
