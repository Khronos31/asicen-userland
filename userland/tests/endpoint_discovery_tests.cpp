// SPDX-License-Identifier: GPL-2.0-only
// Offline coverage for the bounded client runtime-root and named-instance
// discovery APIs in the shared POSIX IPC adapter.
#include "px4/posix_ipc.h"
#include "../../third_party/px4-userland/userland/tests/test_temp_directory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;

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
    explicit TemporaryDirectory(mode_t mode)
    {
        std::string pattern = test::temporary_directory_template("asicen-discovery-");
        char* created = ::mkdtemp(pattern.data());
        if (created != nullptr) {
            path_ = created;
            valid_ = ::chmod(path_.c_str(), mode) == 0;
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

class EnvironmentRestore final {
public:
    explicit EnvironmentRestore(const char* name) : name_(name)
    {
        const char* value = std::getenv(name);
        if (value != nullptr) {
            existed_ = true;
            value_ = value;
        }
    }

    ~EnvironmentRestore() noexcept
    {
        if (existed_) {
            (void)::setenv(name_.c_str(), value_.c_str(), 1);
        } else {
            (void)::unsetenv(name_.c_str());
        }
    }

    EnvironmentRestore(const EnvironmentRestore&) = delete;
    EnvironmentRestore& operator=(const EnvironmentRestore&) = delete;

private:
    std::string name_;
    std::string value_;
    bool existed_ = false;
};

std::string product_directory(const TemporaryDirectory& runtime)
{
    return runtime.path() + "/px4-userland";
}

bool create_private_directory(const std::string& path)
{
    return ::mkdir(path.c_str(), 0700) == 0 && ::chmod(path.c_str(), 0700) == 0;
}

bool discover_finds_published_named_endpoint()
{
    TemporaryDirectory runtime(0700);
    CHECK(runtime.valid());
    const std::string product = product_directory(runtime);
    CHECK(create_private_directory(product));
    CHECK(create_private_directory(product + "/alpha"));
    // A bare instance directory without a control socket is not an endpoint.
    CHECK(create_private_directory(product + "/empty"));

    const EndpointConfig alpha{runtime.path().c_str(), "alpha", kControlEndpointName,
                               EndpointAccess::private_user};
    auto listener = SocketListener::listen(alpha);
    CHECK(listener);

    const auto discovered = discover_runtime_instances(runtime.path().c_str(),
                                                       EndpointAccess::private_user);
    CHECK(discovered);
    CHECK(!discovered.value().canonical_collision);
    CHECK(discovered.value().count == 1U);
    CHECK(std::strcmp(discovered.value().instances[0].data(), "alpha") == 0);

    // The discovered on-disk name connects without another lookup step.
    const EndpointConfig found{runtime.path().c_str(),
                               discovered.value().instances[0].data(),
                               kControlEndpointName, EndpointAccess::private_user};
    auto client = SocketStream::connect(found, Timeout{1000U});
    CHECK(client);
    return true;
}

bool discover_keeps_explicit_root()
{
    TemporaryDirectory runtime(0700);
    CHECK(runtime.valid());
    const std::string product = product_directory(runtime);
    CHECK(create_private_directory(product));
    CHECK(create_private_directory(product + "/named"));

    EnvironmentRestore restore("XDG_RUNTIME_DIR");
    CHECK(::setenv("XDG_RUNTIME_DIR", runtime.path().c_str(), 1) == 0);
    const EndpointConfig named{runtime.path().c_str(), "named", kControlEndpointName,
                               EndpointAccess::private_user};
    auto listener = SocketListener::listen(named);
    CHECK(listener);

    // An explicit missing root reports its own error and never resolves the
    // endpoint published under the default XDG root.
    const std::string missing = runtime.path() + "/missing";
    const auto absent = discover_runtime_instances(missing.c_str(),
                                                   EndpointAccess::private_user);
    CHECK(!absent && absent.error() == Error::NOT_FOUND);

    // An explicit insecure root is rejected instead of being replaced.
    TemporaryDirectory insecure(0755);
    CHECK(insecure.valid());
    const auto wrong_mode = discover_runtime_instances(insecure.path().c_str(),
                                                       EndpointAccess::private_user);
    CHECK(!wrong_mode && wrong_mode.error() == Error::INVALID_ARGUMENT);
    return true;
}

bool discover_reports_case_collision()
{
    TemporaryDirectory runtime(0700);
    CHECK(runtime.valid());
    const std::string product = product_directory(runtime);
    CHECK(create_private_directory(product));
    const std::string upper = product + "/ABC";
    const std::string lower = product + "/abc";
    if (::mkdir(upper.c_str(), 0700) != 0 || ::chmod(upper.c_str(), 0700) != 0) {
        return true;
    }
    if (::mkdir(lower.c_str(), 0700) != 0 || ::chmod(lower.c_str(), 0700) != 0) {
        // A case-insensitive filesystem folded both names into one directory.
        (void)::rmdir(upper.c_str());
        return true;
    }

    const auto discovered = discover_runtime_instances(runtime.path().c_str(),
                                                       EndpointAccess::private_user);
    CHECK(discovered);
    CHECK(discovered.value().canonical_collision);
    return true;
}

bool client_roots_prefer_xdg()
{
    TemporaryDirectory runtime(0700);
    CHECK(runtime.valid());
    EnvironmentRestore restore("XDG_RUNTIME_DIR");
    CHECK(::setenv("XDG_RUNTIME_DIR", runtime.path().c_str(), 1) == 0);
    const auto roots = client_runtime_roots();
    CHECK(roots);
    CHECK(roots.value().count >= 1U);
    CHECK(std::string(roots.value().paths[0].data()) == runtime.path());
    return true;
}

}  // namespace

int main()
{
    if (!discover_finds_published_named_endpoint() ||
        !discover_keeps_explicit_root() ||
        !discover_reports_case_collision() ||
        !client_roots_prefer_xdg()) {
        return 1;
    }
    std::puts("endpoint discovery tests passed (client roots + named instances)");
    return 0;
}
