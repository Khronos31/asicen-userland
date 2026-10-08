// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_TEST_TEMP_DIRECTORY_H
#define PX4_USERLAND_TEST_TEMP_DIRECTORY_H

#include <cstdlib>
#include <string>
#include <string_view>
#include <sys/un.h>

namespace px4::userland::test {

inline std::string temporary_directory_template(std::string_view prefix)
{
    const char* configured_root = std::getenv("TMPDIR");
    std::string root = configured_root != nullptr && configured_root[0] == '/'
                           ? configured_root
                           : "/tmp";

    while (root.size() > 1U && root.back() == '/') {
        root.pop_back();
    }

    // macOS (and some BSDs) reject a pathname that reaches the AF_UNIX
    // sun_path limit.  Keep TMPDIR when the complete test endpoint fits, but
    // use the conventional short temporary root when a runner supplies an
    // overlong absolute TMPDIR.  The IPC boundary itself has a negative
    // overlong-path test in posix_ipc_tests.cpp.
    // The generated directory is the runtime root itself.  Include the
    // complete layout used by the integration tests when checking the
    // platform AF_UNIX budget: runtime/prefixXXXXXX/px4-userland/serial/name.
    constexpr std::string_view kTestInstance = "00001205000960";
    constexpr std::string_view kTestEndpoint = "control.sock";
    std::string endpoint = root;
    if (root != "/") {
        endpoint.push_back('/');
    }
    endpoint.append(prefix);
    endpoint.append("XXXXXX/px4-userland/");
    endpoint.append(kTestInstance);
    endpoint.push_back('/');
    endpoint.append(kTestEndpoint);
    if (endpoint.size() + 1U >= sizeof(sockaddr_un{}.sun_path)) {
        root = "/tmp";
    }

    std::string pattern = root;
    if (root.back() != '/') {
        pattern.push_back('/');
    }
    pattern.append(prefix);
    pattern.append("XXXXXX");
    return pattern;
}

} // namespace px4::userland::test

#endif
