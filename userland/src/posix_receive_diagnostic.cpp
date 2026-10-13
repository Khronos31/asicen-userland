// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/receive_diagnostic.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace asicen {
std::FILE* open_receive_diagnostic_file(const char* path) noexcept
{
    const int descriptor = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return nullptr;
    }
    std::FILE* file = ::fdopen(descriptor, "wb");
    if (file == nullptr) {
        const int error = errno;
        (void)::close(descriptor);
        errno = error;
    }
    return file;
}
}  // namespace asicen
