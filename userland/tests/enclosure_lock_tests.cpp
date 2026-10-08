// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/enclosure_lock.h"

#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void regular_lock_is_created_exclusively_and_existing_inode_reopens() {
    const std::string path = "/tmp/asicen-lock-test-" + std::to_string(::getpid());
    (void)::unlink(path.c_str());
    const int first = asicen::acquire_enclosure_lock(path.c_str());
    check(first >= 0, "lock helper creates and locks an absent path");
    struct stat st{};
    check(::fstat(first, &st) == 0 && S_ISREG(st.st_mode) &&
              (st.st_mode & 0777U) == 0600U,
          "new lock is a regular file with restrictive mode");
    check(asicen::acquire_enclosure_lock(path.c_str()) < 0,
          "second process cannot acquire a held flock");
    check(::close(first) == 0, "release first test lock");

    // Simulate the ctest-user/root-daemon ownership split under /tmp when
    // running as root. Opening this existing inode without O_CREAT avoids
    // fs.protected_regular restrictions; no ownership or system setting is
    // changed outside this uniquely named temporary test file.
    if (::geteuid() == 0) {
        const int raw = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        check(raw >= 0, "open temporary inode for ownership test");
        (void)::fchown(raw, 65534, 65534);
        (void)::close(raw);
    }
    const int reopened = asicen::acquire_enclosure_lock(path.c_str());
    check(reopened >= 0, "existing regular lock is opened without O_CREAT");
    check(::close(reopened) == 0, "release reopened test lock");
    check(::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode),
          "releasing flock keeps the existing lock inode");
    check(::unlink(path.c_str()) == 0, "remove only the unique test artifact");
}

void symlink_and_nonregular_paths_are_rejected() {
    const std::string base = "/tmp/asicen-lock-test-" + std::to_string(::getpid());
    const std::string target = base + "-target";
    const std::string link = base + "-link";
    const std::string directory = base + "-directory";
    (void)::unlink(target.c_str());
    (void)::unlink(link.c_str());
    (void)::rmdir(directory.c_str());
    const int fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    check(fd >= 0, "create temporary symlink target");
    (void)::close(fd);
    check(::symlink(target.c_str(), link.c_str()) == 0,
          "create temporary symlink lock path");
    check(asicen::acquire_enclosure_lock(link.c_str()) < 0,
          "symlink lock path is rejected");
    check(::mkdir(directory.c_str(), 0700) == 0, "create temporary directory path");
    check(asicen::acquire_enclosure_lock(directory.c_str()) < 0,
          "non-regular lock path is rejected");
    check(::unlink(link.c_str()) == 0 && ::unlink(target.c_str()) == 0 &&
              ::rmdir(directory.c_str()) == 0,
          "remove unique non-regular test artifacts");
}
}  // namespace

int main() {
    regular_lock_is_created_exclusively_and_existing_inode_reopens();
    symlink_and_nonregular_paths_are_rejected();
    std::cout << "enclosure lock tests passed\n";
    return 0;
}
