// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_PX4_WINDOWS_ARGS_H
#define PX4_USERLAND_PX4_WINDOWS_ARGS_H

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace px4::userland::cli {

// Windows narrow argv uses the active code page, which irreversibly loses
// characters outside it. Re-derive the arguments from the Unicode command line
// so runtime, firmware, and output paths survive. Returns an empty vector on
// any failure (callers then fail their own argument validation) instead of
// falling back to a lossy ACP string.
inline std::vector<std::string> windows_argv_utf8(int argc, char** argv) noexcept
{
    (void)argc;
    (void)argv;
    int wide_count = 0;
    LPWSTR* wide_argv = ::CommandLineToArgvW(::GetCommandLineW(), &wide_count);
    if (wide_argv == nullptr || wide_count <= 0) {
        if (wide_argv != nullptr) {
            ::LocalFree(wide_argv);
        }
        return {};
    }
    std::vector<std::string> converted;
    converted.reserve(static_cast<std::size_t>(wide_count));
    for (int index = 0; index < wide_count; ++index) {
        const int length = ::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[index], -1, nullptr, 0, nullptr,
            nullptr);
        if (length <= 0) {
            ::LocalFree(wide_argv);
            return {};
        }
        std::string utf8(static_cast<std::size_t>(length), '\0');
        if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[index], -1,
                                  utf8.data(), length, nullptr, nullptr) != length) {
            ::LocalFree(wide_argv);
            return {};
        }
        utf8.resize(static_cast<std::size_t>(length - 1));
        converted.push_back(std::move(utf8));
    }
    ::LocalFree(wide_argv);
    return converted;
}

// Converts one UTF-8 path to a null-terminated wide path for Win32 file APIs.
// Returns an empty vector for empty or invalid UTF-8 input.
inline std::vector<wchar_t> utf8_to_wide_path(const std::string& utf8) noexcept
{
    if (utf8.empty()) {
        return {};
    }
    const int length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(length) + 1U, L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(),
                              static_cast<int>(utf8.size()), wide.data(),
                              length) != length) {
        return {};
    }
    return wide;
}

}  // namespace px4::userland::cli

#endif  // _WIN32

#endif  // PX4_USERLAND_PX4_WINDOWS_ARGS_H
