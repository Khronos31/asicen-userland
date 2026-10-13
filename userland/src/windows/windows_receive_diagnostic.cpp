// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/receive_diagnostic.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "windows/windows_security.h"
#include <cerrno>
#include <fcntl.h>
#include <io.h>

namespace asicen {
std::FILE* open_receive_diagnostic_file(const char* path) noexcept
{
    SECURITY_ATTRIBUTES attributes{};
    SECURITY_DESCRIPTOR security{};
    PACL acl = nullptr;
    px4::userland::windows_security::SidBuffer sid_storage{};
    if (!px4::userland::windows_security::build_current_user_security(
            attributes, security, acl, sid_storage)) {
        errno = EACCES;
        return nullptr;
    }
    // Match getenv's native narrow Windows path encoding. CREATE_NEW refuses
    // existing files (including reparse points), and the DACL is user-only.
    HANDLE handle = ::CreateFileA(path, GENERIC_WRITE, 0U, &attributes, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD error = ::GetLastError();
    (void)::LocalFree(acl);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? EEXIST
                : error == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
        return nullptr;
    }
    const int descriptor = ::_open_osfhandle(reinterpret_cast<std::intptr_t>(handle),
                                             _O_WRONLY | _O_BINARY | _O_NOINHERIT);
    if (descriptor < 0) {
        (void)::CloseHandle(handle);
        return nullptr;
    }
    std::FILE* file = ::_fdopen(descriptor, "wb");
    if (file == nullptr) {
        const int saved_error = errno;
        (void)::_close(descriptor);
        errno = saved_error;
    }
    return file;
}
}  // namespace asicen
