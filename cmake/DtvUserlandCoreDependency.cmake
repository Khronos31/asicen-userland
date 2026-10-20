# SPDX-License-Identifier: GPL-2.0-only
#
# Locate the immutable MIT dtv-userland-core foundation library.
#
# The dependency is pinned by a full 40-hex commit recorded in
# cmake/dtv-userland-core.revision.  A corresponding-source archive may bundle
# the tree under third_party/dtv-userland-core.  For local or modified builds,
# set FETCHCONTENT_SOURCE_DIR_DTV_USERLAND_CORE (or DTV_USERLAND_CORE_SOURCE_DIR)
# to an explicit source tree; such a build is reported as unpinned and never
# silently falls back to a duplicated implementation.

include_guard(GLOBAL)

include(FetchContent)

set(DTV_USERLAND_CORE_REPOSITORY
    "git@github.com:Khronos31/dtv-userland-core.git"
    CACHE STRING "Git repository for the dtv-userland-core dependency")
set(DTV_USERLAND_CORE_SOURCE_DIR "" CACHE PATH
    "Explicit local dtv-userland-core source tree (local/modified override)")

function(asicen_dtv_userland_core_revision out_var)
    set(_file "${CMAKE_CURRENT_SOURCE_DIR}/cmake/dtv-userland-core.revision")
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR
            "missing dtv-userland-core lock file: ${_file}")
    endif()
    file(READ "${_file}" _contents)
    string(STRIP "${_contents}" _revision)
    if(_revision MATCHES "[\r\n]" OR NOT _revision MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR
            "${_file} must contain exactly one full 40-hex commit")
    endif()
    string(TOLOWER "${_revision}" _revision)
    string(LENGTH "${_revision}" _length)
    if(NOT _length EQUAL 40 OR NOT _revision MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "${_file} must contain a full 40-hex commit, got '${_revision}'")
    endif()
    if(_revision STREQUAL "0000000000000000000000000000000000000000")
        message(FATAL_ERROR
            "${_file} is not yet pinned; replace the placeholder with the "
            "verified common commit before a non-override build")
    endif()
    set(${out_var} "${_revision}" PARENT_SCOPE)
endfunction()

function(asicen_add_dtv_userland_core)
    set(_bundled "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dtv-userland-core")

    if(DTV_USERLAND_CORE_SOURCE_DIR)
        set(_override "${DTV_USERLAND_CORE_SOURCE_DIR}")
    elseif(FETCHCONTENT_SOURCE_DIR_DTV_USERLAND_CORE)
        set(_override "${FETCHCONTENT_SOURCE_DIR_DTV_USERLAND_CORE}")
    else()
        set(_override "")
    endif()

    if(_override)
        if(NOT EXISTS "${_override}/CMakeLists.txt")
            message(FATAL_ERROR
                "dtv-userland-core source override has no CMakeLists.txt: ${_override}")
        endif()
        message(WARNING
            "dtv-userland-core: using explicit local source ${_override}; "
            "this build is not pinned to cmake/dtv-userland-core.revision")
        FetchContent_Declare(dtv_userland_core SOURCE_DIR "${_override}")
    elseif(EXISTS "${_bundled}/CMakeLists.txt")
        asicen_dtv_userland_core_revision(_revision)
        message(STATUS
            "dtv-userland-core: using bundled corresponding source ${_bundled}")
        FetchContent_Declare(dtv_userland_core SOURCE_DIR "${_bundled}")
    else()
        asicen_dtv_userland_core_revision(_revision)
        message(STATUS
            "dtv-userland-core: fetching pinned commit ${_revision}")
        FetchContent_Declare(dtv_userland_core
            GIT_REPOSITORY "${DTV_USERLAND_CORE_REPOSITORY}"
            GIT_TAG "${_revision}")
    endif()

    # The foundation's own tests must never turn on as product tests.
    set(_saved_build_testing "${BUILD_TESTING}")
    set(BUILD_TESTING OFF)
    set(DTV_USERLAND_BUILD_RECEIVER ON)
    set(DTV_USERLAND_BUILD_CARD ON)
    set(DTV_USERLAND_BUILD_HOST_CTL ON CACHE BOOL
        "Build the optional GPL host control frontend" FORCE)
    set(DTV_USERLAND_HOST_CTL_FACADE_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/px4-userland/userland/include"
        CACHE STRING
        "Private host facade include directories required by the GPL host control frontend"
        FORCE)
    FetchContent_MakeAvailable(dtv_userland_core)
    set(BUILD_TESTING "${_saved_build_testing}")
    FetchContent_GetProperties(dtv_userland_core SOURCE_DIR _dtv_userland_core_source_dir)
    if(NOT _dtv_userland_core_source_dir)
        message(FATAL_ERROR "dtv-userland-core source directory was not populated")
    endif()
    set(ASICEN_DTV_CORE_SOURCE_DIR "${_dtv_userland_core_source_dir}" PARENT_SCOPE)

    if(NOT TARGET dtv::core)
        message(FATAL_ERROR "dtv-userland-core did not define the dtv::core target")
    endif()
    if(NOT TARGET dtv::receiver)
        message(FATAL_ERROR
            "dtv-userland-core did not define the dtv::receiver target; "
            "the pinned commit predates the receiver lifecycle engine")
    endif()
    if(NOT TARGET dtv::card)
        message(FATAL_ERROR
            "dtv-userland-core did not define the dtv::card target; "
            "the pinned commit predates the card ownership engine")
    endif()
    if(NOT TARGET dtv::control)
        message(FATAL_ERROR
            "dtv-userland-core did not define the dtv::control target; "
            "the pinned commit predates the control connection")
    endif()
    if(NOT TARGET dtv::host_ctl)
        message(FATAL_ERROR
            "dtv-userland-core did not define the optional GPL host control frontend")
    endif()
endfunction()
