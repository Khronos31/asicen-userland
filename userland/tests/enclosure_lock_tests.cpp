// SPDX-License-Identifier: GPL-2.0-only
// Original enclosure-lock safety coverage migrated to the shared endpoint lease.
#include "px4/posix_ipc.h"
#include "px4/endpoint_diagnostic.h"
#include "../tools/asicend_identity_lease.h"
#include "../../third_party/px4-userland/userland/tests/test_temp_directory.h"

#include <cstdlib>
#include <cstdio>
#include <array>
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
        EndpointDiagnostic owner_diagnostic{};
        const auto foreign_owner = SerialEndpointLease::acquire_identity(
            endpoint, kIdentity, &owner_diagnostic);
        CHECK(!foreign_owner && foreign_owner.error() == Error::INVALID_ARGUMENT);
        CHECK(owner_diagnostic.reason == EndpointDiagnosticReason::lease_file_owner);
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
    EndpointDiagnostic lock_mode_diagnostic{};
    const auto lock_mode = SerialEndpointLease::acquire_identity(
        endpoint, kIdentity, &lock_mode_diagnostic);
    CHECK(!lock_mode && lock_mode.error() == Error::INVALID_ARGUMENT);
    CHECK(lock_mode_diagnostic.reason == EndpointDiagnosticReason::lease_file_mode);
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

bool endpoint_length_and_runtime_mode_diagnostics_are_specific()
{
    TemporaryDirectory directory;
    CHECK(directory.valid());
    const EndpointConfig endpoint{directory.path().c_str(), "test", kControlEndpointName};
    const std::string overlong_runtime = "/" + std::string(96U, 'x');
    const EndpointConfig overlong{overlong_runtime.c_str(), "test", kControlEndpointName};
    EndpointDiagnostic path_diagnostic{};
    const auto too_long = SerialEndpointLease::acquire_identity(
        overlong, kIdentity, &path_diagnostic);
    CHECK(!too_long && too_long.error() == Error::INVALID_ARGUMENT);
    CHECK(path_diagnostic.reason == EndpointDiagnosticReason::path_too_long);
    CHECK(path_diagnostic.path_bytes + 1U == path_diagnostic.required_bytes);
    CHECK(path_diagnostic.required_bytes > path_diagnostic.native_capacity);

    const EndpointConfig invalid_instance{directory.path().c_str(), "", kControlEndpointName};
    EndpointDiagnostic instance_diagnostic{};
    const auto invalid = SerialEndpointLease::acquire_identity(
        invalid_instance, kIdentity, &instance_diagnostic);
    CHECK(!invalid && invalid.error() == Error::INVALID_ARGUMENT);
    CHECK(instance_diagnostic.reason == EndpointDiagnosticReason::instance);

    if (::geteuid() == 0) {
        CHECK(::chown(directory.path().c_str(), 65534, 65534) == 0);
        EndpointDiagnostic owner_diagnostic{};
        const auto unsafe_owner = SerialEndpointLease::acquire_identity(
            endpoint, kIdentity, &owner_diagnostic);
        CHECK(!unsafe_owner && unsafe_owner.error() == Error::INVALID_ARGUMENT);
        CHECK(owner_diagnostic.reason == EndpointDiagnosticReason::runtime_directory_owner);
        CHECK(::chown(directory.path().c_str(), 0, 0) == 0);
    }

    CHECK(::chmod(directory.path().c_str(), 0755) == 0);
    EndpointDiagnostic mode_diagnostic{};
    const auto unsafe_mode = SerialEndpointLease::acquire_identity(
        endpoint, kIdentity, &mode_diagnostic);
    CHECK(!unsafe_mode && unsafe_mode.error() == Error::INVALID_ARGUMENT);
    CHECK(mode_diagnostic.reason == EndpointDiagnosticReason::runtime_directory_mode);
    CHECK(::chmod(directory.path().c_str(), 0700) == 0);
    return true;
}

bool endpoint_diagnostic_text_keeps_bytes_and_remediation()
{
    EndpointDiagnostic diagnostic{};
    diagnostic.reason = EndpointDiagnosticReason::path_too_long;
    diagnostic.path_bytes = 113U;
    diagnostic.required_bytes = 114U;
    diagnostic.native_capacity = 108U;
    FILE* output = std::tmpfile();
    CHECK(output != nullptr);
    asicen::cli::print_endpoint_diagnostic(output, diagnostic, false);
    CHECK(std::fflush(output) == 0);
    std::rewind(output);
    std::array<char, 512U> text{};
    const std::size_t size = std::fread(text.data(), 1U, text.size() - 1U, output);
    CHECK(std::fclose(output) == 0);
    const std::string message(text.data(), size);
    CHECK(message.find("113 UTF-8 bytes") != std::string::npos);
    CHECK(message.find("114 bytes including NUL required") != std::string::npos);
    CHECK(message.find("native sun_path capacity 108 bytes including NUL") != std::string::npos);
    CHECK(message.find("shorten --runtime-dir or --instance") != std::string::npos);
    return true;
}

bool migrated_identity_lease_matches_legacy_private_and_group_owner_policy()
{
    TemporaryDirectory directory;
    CHECK(directory.valid());
    const std::string& runtime = directory.path();
    const EndpointConfig private_endpoint{
        runtime.c_str(), "first", kControlEndpointName, EndpointAccess::private_user};
    auto legacy_private = SerialEndpointLease::acquire_identity(private_endpoint, kIdentity);
    CHECK(legacy_private);
    legacy_private.value().close();
    auto migrated_private = asicen::cli::acquire_identity_lease(runtime, false, kIdentity);
    CHECK(migrated_private);
    migrated_private.value().close();

    CHECK(::chmod(runtime.c_str(), 0750) == 0);
    const EndpointConfig group_endpoint{
        runtime.c_str(), "second", kControlEndpointName, EndpointAccess::shared_group};
    auto legacy_group = SerialEndpointLease::acquire_identity(group_endpoint, kIdentity);
    CHECK(legacy_group);
    legacy_group.value().close();
    auto migrated_group = asicen::cli::acquire_identity_lease(runtime, true, kIdentity);
    CHECK(migrated_group);
    const auto same_identity = asicen::cli::acquire_identity_lease(
        runtime, true, kIdentity);
    CHECK(!same_identity && same_identity.error() == dtv::userland::Error::BUSY);
    auto independent_identity = asicen::cli::acquire_identity_lease(
        runtime, true, "usb-1-3");
    CHECK(independent_identity);
    independent_identity.value().close();
    migrated_group.value().close();
    auto reacquired_group = asicen::cli::acquire_identity_lease(
        runtime, true, kIdentity);
    CHECK(reacquired_group);
    reacquired_group.value().close();

    if (::geteuid() == 0 && ::getegid() != 65534) {
        CHECK(::chown(runtime.c_str(), ::geteuid(), 65534) == 0);
        auto legacy_other_gid = SerialEndpointLease::acquire_identity(
            group_endpoint, kIdentity);
        CHECK(legacy_other_gid);
        legacy_other_gid.value().close();
        auto migrated_other_gid = asicen::cli::acquire_identity_lease(
            runtime, true, kIdentity);
        CHECK(migrated_other_gid);
        migrated_other_gid.value().close();
        CHECK(::chown(runtime.c_str(), ::geteuid(), ::getegid()) == 0);
    }

    CHECK(::chmod(runtime.c_str(), 0700) == 0);
    CHECK(!SerialEndpointLease::acquire_identity(group_endpoint, kIdentity));
    CHECK(!asicen::cli::acquire_identity_lease(runtime, true, kIdentity));
    CHECK(::chmod(runtime.c_str(), 0750) == 0);

    if (::geteuid() == 0) {
        CHECK(::chown(runtime.c_str(), 65534, ::getegid()) == 0);
        CHECK(!SerialEndpointLease::acquire_identity(group_endpoint, kIdentity));
        CHECK(!asicen::cli::acquire_identity_lease(runtime, true, kIdentity));
        CHECK(::chown(runtime.c_str(), ::geteuid(), ::getegid()) == 0);
    }

    const std::string missing = runtime + "/missing";
    const EndpointConfig missing_endpoint{
        missing.c_str(), "third", kControlEndpointName, EndpointAccess::shared_group};
    CHECK(!SerialEndpointLease::acquire_identity(missing_endpoint, kIdentity));
    CHECK(!asicen::cli::acquire_identity_lease(missing, true, kIdentity));
    CHECK(::access(missing.c_str(), F_OK) != 0);
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
        !symlink_and_nonregular_paths_are_rejected() ||
        !endpoint_length_and_runtime_mode_diagnostics_are_specific() ||
        !endpoint_diagnostic_text_keeps_bytes_and_remediation() ||
        !migrated_identity_lease_matches_legacy_private_and_group_owner_policy()) {
        return 1;
    }
    std::puts("enclosure lock tests passed (shared endpoint lease)");
    return 0;
}
