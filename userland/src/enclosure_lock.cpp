// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/enclosure_lock.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace asicen {

int acquire_enclosure_lock(const char* path) noexcept {
    if (path == nullptr || *path == '\0') return -1;

    int fd = -1;
    for (unsigned int attempt = 0; attempt < 3U; ++attempt) {
        fd = ::open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) break;
        if (errno != ENOENT) return -1;

        fd = ::open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0600);
        if (fd >= 0) break;
        if (errno != EEXIST) return -1;
        // Another process created the inode after our ENOENT. Reopen it
        // without O_CREAT so sticky-directory ownership checks are respected.
    }
    if (fd < 0) return -1;

    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        (void)::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace asicen
