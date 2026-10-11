// SPDX-License-Identifier: GPL-2.0-only
// Original enclosure-lock safety coverage migrated to the shared endpoint lease.
#include "px4/posix_ipc.h"
#include "../../third_party/px4-userland/userland/tests/test_temp_directory.h"

#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc::posix;
constexpr const char* kIdentity = "usb-1-2";

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,         \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        std::string pattern = test::temporary_directory_template("asicen-lock-");
        char* created = ::mkdtemp(pattern.data());
        if (created != nullptr) {
            path_ = created;
            valid_ = ::chmod(path_.c_str(), 0700) == 0;
        }
    }

    ~TemporaryDirectory() noexcept
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    bool valid() const noexcept { return valid_; }
    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    bool valid_ = false;
};

class ScopedFd final {
public:
    explicit ScopedFd(int fd) noexcept : fd_(fd) {}
    ~ScopedFd() noexcept { (void)close(); }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const noexcept { return fd_; }
    int close() noexcept
    {
        const int fd = fd_;
        fd_ = -1;
        return fd >= 0 ? ::close(fd) : 0;
    }

private:
    int fd_ = -1;
};

int create_lock(const std::string& path)
{
    return ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
}

bool regular_lock_is_created_exclusively_and_existing_inode_reopens()
{
    TemporaryDirectory directory;
    CHECK(directory.valid());
    const std::string path = directory.path() + "/.asicen-userland-usb-1-2.lock";
    const EndpointConfig endpoint{directory.path().c_str(), "first", kControlEndpointName};
    const EndpointConfig other{directory.path().c_str(), "second", kControlEndpointName};
    auto first = SerialEndpointLease::acquire_identity(endpoint, kIdentity);
    CHECK(first && first.value().valid());
    struct stat status {};
    CHECK(::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
          (status.st_mode & 07777U) == 0600U);
    CHECK(SerialEndpointLease::acquire_identity(other, kIdentity).error() == Error::BUSY);
    first.value().close();
    CHECK(::lstat(path.c_str(), &status) != 0);

    ScopedFd raw(create_lock(path));
    CHECK(raw.get() >= 0);
    // A privileged daemon must not adopt another user's pre-created inode.
    // Only the inode in this mkdtemp-created private directory is changed.
    if (::geteuid() == 0) {
        CHECK(::fchown(raw.get(), 65534, 65534) == 0);
        CHECK(SerialEndpointLease::acquire_identity(endpoint, kIdentity).error() ==
              Error::INVALID_ARGUMENT);
        CHECK(::fchown(raw.get(), 0, 0) == 0);
    }
    CHECK(raw.close() == 0);
    auto reopened = SerialEndpointLease::acquire_identity(endpoint, kIdentity);
    CHECK(reopened && reopened.value().valid());
    reopened.value().close();
    CHECK(::lstat(path.c_str(), &status) != 0);

    ScopedFd invalid_mode(create_lock(path));
    CHECK(invalid_mode.get() >= 0);
    CHECK(invalid_mode.close() == 0);
    CHECK(::chmod(path.c_str(), 0666) == 0);
    CHECK(SerialEndpointLease::acquire_identity(endpoint, kIdentity).error() ==
          Error::INVALID_ARGUMENT);
    CHECK(::chmod(path.c_str(), 0600) == 0);
    const std::string alias = path + "-alias";
    CHECK(::link(path.c_str(), alias.c_str()) == 0);
    CHECK(SerialEndpointLease::acquire_identity(endpoint, kIdentity).error() ==
          Error::INVALID_ARGUMENT);
    CHECK(::unlink(alias.c_str()) == 0);
    CHECK(::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode));
    CHECK(::unlink(path.c_str()) == 0);
    CHECK(::rmdir(directory.path().c_str()) == 0);
    return true;
}

bool symlink_and_nonregular_paths_are_rejected()
{
    TemporaryDirectory directory;
    CHECK(directory.valid());
    const std::string target = directory.path() + "/target";
    const std::string path = directory.path() + "/.asicen-userland-usb-1-2.lock";
    const EndpointConfig endpoint{directory.path().c_str(), "test", kControlEndpointName};
    ScopedFd raw(create_lock(target));
    CHECK(raw.get() >= 0);
    CHECK(raw.close() == 0);
    CHECK(::symlink(target.c_str(), path.c_str()) == 0);
    CHECK(!SerialEndpointLease::acquire_identity(endpoint, kIdentity));
    struct stat status {};
    CHECK(::lstat(target.c_str(), &status) == 0 && S_ISREG(status.st_mode));
    CHECK(::unlink(path.c_str()) == 0);
    CHECK(::mkdir(path.c_str(), 0700) == 0);
    CHECK(!SerialEndpointLease::acquire_identity(endpoint, kIdentity));
    CHECK(::unlink(target.c_str()) == 0 && ::rmdir(path.c_str()) == 0);
    CHECK(::rmdir(directory.path().c_str()) == 0);
    return true;
}

} // namespace

int main()
{
    if (!regular_lock_is_created_exclusively_and_existing_inode_reopens() ||
        !symlink_and_nonregular_paths_are_rejected()) {
        return 1;
    }
    std::puts("enclosure lock tests passed (shared endpoint lease)");
    return 0;
}
