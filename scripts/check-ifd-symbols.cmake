# SPDX-License-Identifier: GPL-2.0-only
if(NOT DEFINED NM OR NOT DEFINED LIBRARY)
    message(FATAL_ERROR "NM and LIBRARY are required")
endif()

if(IS_APPLE)
    execute_process(
        COMMAND "${NM}" -gU "${LIBRARY}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE symbols
        ERROR_VARIABLE error_output
    )
else()
    execute_process(
        COMMAND "${NM}" -D --defined-only "${LIBRARY}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE symbols
        ERROR_VARIABLE error_output
    )
endif()
if(NOT result EQUAL 0)
    message(FATAL_ERROR "nm failed: ${error_output}")
endif()

if(IS_APPLE)
    find_program(dependency_tool NAMES otool REQUIRED)
    set(dependency_arguments -L)
else()
    find_program(dependency_tool NAMES readelf REQUIRED)
    set(dependency_arguments -d)
endif()
execute_process(
    COMMAND "${dependency_tool}" ${dependency_arguments} "${LIBRARY}"
    RESULT_VARIABLE dependency_result
    OUTPUT_VARIABLE dependencies
    ERROR_VARIABLE dependency_error
)
if(NOT dependency_result EQUAL 0)
    message(FATAL_ERROR "dependency inspection failed: ${dependency_error}")
endif()
if(dependencies MATCHES "PCSC\\.framework|libpcsclite")
    message(FATAL_ERROR "IFD Handler must not link the PC/SC client library:\n${dependencies}")
endif()

if(IS_APPLE)
    if(NOT DEFINED BUNDLE OR NOT IS_DIRECTORY "${BUNDLE}")
        message(FATAL_ERROR "macOS IFD bundle directory not found: ${BUNDLE}")
    endif()
    set(bundle_info "${BUNDLE}/Contents/Info.plist")
    get_filename_component(library_name "${LIBRARY}" NAME)
    set(bundle_library "${BUNDLE}/Contents/MacOS/${library_name}")
    if(NOT EXISTS "${bundle_info}" OR NOT EXISTS "${bundle_library}")
        message(FATAL_ERROR "incomplete macOS IFD bundle: ${BUNDLE}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${LIBRARY}" "${bundle_library}"
        RESULT_VARIABLE bundle_library_result
    )
    if(NOT bundle_library_result EQUAL 0)
        message(FATAL_ERROR "macOS IFD bundle executable differs from target")
    endif()
    find_program(plutil NAMES plutil REQUIRED)
    execute_process(
        COMMAND "${plutil}" -lint "${bundle_info}"
        RESULT_VARIABLE plist_result
        OUTPUT_VARIABLE plist_output
        ERROR_VARIABLE plist_error
    )
    if(NOT plist_result EQUAL 0)
        message(FATAL_ERROR "invalid macOS IFD Info.plist: ${plist_output}${plist_error}")
    endif()
    file(READ "${bundle_info}" plist)
    foreach(required_key CFBundleExecutable CFBundleIdentifier CFBundlePackageType)
        if(NOT plist MATCHES "<key>${required_key}</key>")
            message(FATAL_ERROR "macOS IFD Info.plist lacks ${required_key}")
        endif()
    endforeach()
    if(NOT plist MATCHES "<string>${library_name}</string>" OR
       NOT plist MATCHES "<string>BNDL</string>")
        message(FATAL_ERROR "macOS IFD Info.plist has incorrect executable or package type")
    endif()
    foreach(forbidden_key ifdVendorID ifdProductID IOKitPersonalities)
        if(plist MATCHES "<key>${forbidden_key}</key>")
            message(FATAL_ERROR "macOS IFD bundle must not declare ${forbidden_key}")
        endif()
    endforeach()
endif()

set(expected
    IFDHCreateChannel
    IFDHCreateChannelByName
    IFDHCloseChannel
    IFDHGetCapabilities
    IFDHSetCapabilities
    IFDHSetProtocolParameters
    IFDHPowerICC
    IFDHTransmitToICC
    IFDHControl
    IFDHICCPresence
)
foreach(symbol IN LISTS expected)
    if(NOT symbols MATCHES "(^|[ \t\n])_?${symbol}([ \t\n]|$)")
        message(FATAL_ERROR "missing exported IFD symbol: ${symbol}\n${symbols}")
    endif()
endforeach()

string(REGEX MATCHALL "_?IFDH[A-Za-z0-9_]+" exported_ifd_symbols "${symbols}")
list(LENGTH exported_ifd_symbols exported_count)
if(NOT exported_count EQUAL 10)
    message(FATAL_ERROR "unexpected IFD ABI symbol set:\n${symbols}")
endif()

string(REPLACE "\n" ";" symbol_lines "${symbols}")
foreach(line IN LISTS symbol_lines)
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX REPLACE "^.*[ \t]([^ \t]+)$" "\\1" symbol_name "${line}")
    if(NOT symbol_name MATCHES "^_?IFDH[A-Za-z0-9_]+$")
        message(FATAL_ERROR "non-ABI symbol exported: ${symbol_name}\n${symbols}")
    endif()
endforeach()
