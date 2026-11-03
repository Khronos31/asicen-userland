// SPDX-License-Identifier: GPL-2.0-or-later
#include "../tools/asicend_identity_lease.h"
#include "asicen/px4_mock_backend.h"
#include "../../third_party/px4-userland/userland/tests/test_temp_directory.h"
#include <dtv/userland/host_runtime/posix_ipc.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace px4::userland::ipc::posix;

#define CHECK(condition)                                                        \
    do {                                                                         \
        if (!(condition)) {                                                      \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,      \
                         __LINE__, #condition);                                 \
            return false;                                                        \
        }                                                                        \
    } while (false)

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        std::string pattern =
            px4::userland::test::temporary_directory_template("asicen-runtime-");
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

    bool valid() const noexcept { return valid_; }
    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    bool valid_ = false;
};

class ScopedEnvironment final {
public:
    explicit ScopedEnvironment(const char* name) : name_(name)
    {
        const char* value = std::getenv(name_.c_str());
        if (value != nullptr) {
            existed_ = true;
            previous_ = value;
        }
    }

    ~ScopedEnvironment() noexcept
    {
        if (existed_) (void)::setenv(name_.c_str(), previous_.c_str(), 1);
        else (void)::unsetenv(name_.c_str());
    }

    bool set(const std::string& value) noexcept
    {
        return ::setenv(name_.c_str(), value.c_str(), 1) == 0;
    }

private:
    std::string name_;
    std::string previous_;
    bool existed_ = false;
};

bool create_directory(const std::string& path)
{
    return ::mkdir(path.c_str(), 0700) == 0;
}

bool default_runtime_and_identity_lease_use_the_core_candidate_policy()
{
    TemporaryDirectory directory;
    CHECK(directory.valid());
    const std::string xdg_parent = directory.path() + "/" + std::string(25U, 'x');
    const std::string xdg_runtime = xdg_parent + "/runtime";
    const std::string tmpdir = directory.path() + "/tmp";
    CHECK(create_directory(xdg_parent));
    CHECK(create_directory(xdg_runtime));
    CHECK(create_directory(tmpdir));

    ScopedEnvironment saved_xdg("XDG_RUNTIME_DIR");
    ScopedEnvironment saved_tmpdir("TMPDIR");
    CHECK(saved_xdg.set(xdg_runtime));
    CHECK(saved_tmpdir.set(tmpdir));

    const auto short_endpoint = select_publish_runtime_root("asicen-userland", "a");
    const auto long_endpoint = select_publish_runtime_root(
        "asicen-userland", "12345678901234567890");
    const auto lease_root = select_lease_runtime_root();
    CHECK(short_endpoint && long_endpoint && lease_root);
    const std::string canonical_xdg = std::filesystem::canonical(xdg_runtime).string();
    CHECK(short_endpoint.value() == canonical_xdg);
    CHECK(long_endpoint.value() != canonical_xdg);
    CHECK(lease_root.value() == canonical_xdg);

    asicen::MockTunerBackend hardware(4U);
    auto first = asicen::cli::acquire_identity_lease({}, false, "usb-1-2");
    CHECK(first);
    int mock_hardware_prepares = 0;
    ++mock_hardware_prepares;
    CHECK(hardware.open_receiver(0U));
    const auto second = asicen::cli::acquire_identity_lease({}, false, "usb-1-2");
    if (second) {
        ++mock_hardware_prepares;
        CHECK(hardware.open_receiver(0U));
    }
    CHECK(!second && second.error() == dtv::userland::Error::BUSY);
    CHECK(mock_hardware_prepares == 1);

    first.value().close();
    std::puts("ASICEN automatic runtime and identity lease adoption passed");
    return true;
}

}  // namespace

int main()
{
    return default_runtime_and_identity_lease_use_the_core_candidate_policy() ? 0 : 1;
}
