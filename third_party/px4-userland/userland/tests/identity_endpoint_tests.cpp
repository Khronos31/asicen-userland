// SPDX-License-Identifier: GPL-2.0-only
// ASICEN serial-free entry to the pinned endpoint lease implementation.
#include "px4/posix_ipc.h"
#include "test_temp_directory.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc::posix;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n",             \
                         __FILE__, __LINE__, #condition);                    \
            return false;                                                   \
        }                                                                   \
    } while (false)

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        std::string pattern = test::temporary_directory_template("asicen-lease-");
        const char* created = ::mkdtemp(pattern.data());
        if (created != nullptr) path_ = created;
    }
    ~TemporaryDirectory() noexcept
    {
        std::error_code error;
        if (!path_.empty()) std::filesystem::remove_all(path_, error);
    }
    const std::string& path() const noexcept { return path_; }
private:
    std::string path_;
};

bool test_identity_lease()
{
    TemporaryDirectory runtime;
    CHECK(!runtime.path().empty());
    const EndpointConfig first{runtime.path().c_str(), "left", kControlEndpointName};
    const EndpointConfig second{runtime.path().c_str(), "right", kControlEndpointName};
    constexpr std::string_view identity = "usb1-2.1";
    const std::string lock = runtime.path() + "/.asicen-userland-usb1-2.1.lock";
    for (const std::string_view invalid : {"", ".", "..", "../usb", "a/b", "a\\b", "a:b"}) {
        CHECK(SerialEndpointLease::acquire_identity(first, invalid).error() ==
              Error::INVALID_ARGUMENT);
    }
    const std::string embedded("usb\0other", 9U);
    CHECK(SerialEndpointLease::acquire_identity(first, embedded).error() ==
          Error::INVALID_ARGUMENT);
    CHECK(SerialEndpointLease::acquire_identity(first, std::string(512U, 'a')).error() ==
          Error::INVALID_ARGUMENT);
    CHECK(SerialEndpointLease::acquire(first, identity).error() == Error::INVALID_ARGUMENT);
    auto lease = SerialEndpointLease::acquire_identity(first, identity);
    CHECK(lease && lease.value().valid());
    struct stat status{};
    CHECK(::stat(lock.c_str(), &status) == 0 && (status.st_mode & 07777) == 0600);
    CHECK(SerialEndpointLease::acquire_identity(second, identity).error() == Error::BUSY);
    auto other = SerialEndpointLease::acquire_identity(second, "usb1-3.1");
    CHECK(other);
    auto moved = std::move(lease.value());
    CHECK(moved.valid() && !lease.value().valid());
    CHECK(SerialEndpointLease::acquire_identity(second, identity).error() == Error::BUSY);
    moved.close();
    moved.close();
    CHECK(!std::filesystem::exists(lock));
    auto after = SerialEndpointLease::acquire_identity(second, identity);
    CHECK(after);
    after.value().close();
    other.value().close();
    CHECK(std::filesystem::is_empty(runtime.path()));

    // The identity entrance must retain the original mode/symlink/inode guards.
    const int unsafe = ::open(lock.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(unsafe >= 0);
    CHECK(::fchmod(unsafe, 0644) == 0);
    CHECK(::close(unsafe) == 0);
    CHECK(SerialEndpointLease::acquire_identity(first, identity).error() ==
          Error::INVALID_ARGUMENT);
    CHECK(::unlink(lock.c_str()) == 0);
    const std::string target = runtime.path() + "/target";
    const int target_fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(target_fd >= 0 && ::close(target_fd) == 0);
    CHECK(::symlink(target.c_str(), lock.c_str()) == 0);
    CHECK(!SerialEndpointLease::acquire_identity(first, identity));
    CHECK(std::filesystem::exists(target));
    CHECK(::unlink(lock.c_str()) == 0);
    CHECK(::unlink(target.c_str()) == 0);
    CHECK(::chmod(runtime.path().c_str(), 0777) == 0);
    CHECK(!SerialEndpointLease::acquire_identity(first, identity));
    CHECK(::chmod(runtime.path().c_str(), 0700) == 0);
    return true;
}

}  // namespace

int main()
{
    if (!test_identity_lease()) {
        std::fprintf(stderr, "FAIL identity_endpoint_lease\n");
        return 1;
    }
    std::printf("PASS identity_endpoint_lease\n");
    return 0;
}
