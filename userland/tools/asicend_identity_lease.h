// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_ASICEND_IDENTITY_LEASE_H
#define ASICEN_USERLAND_ASICEND_IDENTITY_LEASE_H

#include "dtv/userland/host_listener_context/identity_lease.h"
#include <string>

#include <cstdio>
#include <string_view>

namespace px4::userland::ipc::posix {
dtv::userland::Result<std::string> select_lease_runtime_root() noexcept;
}

namespace asicen::cli {

using dtv::userland::Error;
using dtv::userland::Result;
using dtv::userland::host_listener_context::IdentityLease;
using dtv::userland::host_listener_context::IdentityLeaseConfig;
using dtv::userland::host_listener_context::IdentityLeaseDiagnostic;
using dtv::userland::host_listener_context::IdentityLeaseDiagnosticReason;
using dtv::userland::host_runtime::EndpointAccess;

inline Result<IdentityLease> acquire_identity_lease(
    std::string_view runtime_directory, bool group, std::string_view identity,
    IdentityLeaseDiagnostic* diagnostic = nullptr) noexcept
{
#if defined(_WIN32)
    if (group) return Result<IdentityLease>::failure(Error::UNSUPPORTED);
#endif
    std::string selected_runtime_directory;
    if (runtime_directory.empty()) {
#if defined(ASICEN_TEST_NO_AUTOMATIC_RUNTIME_DEFAULT)
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
#else
        auto selected = px4::userland::ipc::posix::select_lease_runtime_root();
        if (!selected) return Result<IdentityLease>::failure(selected.error());
        selected_runtime_directory = selected.value();
        runtime_directory = selected_runtime_directory;
#endif
    }
    const IdentityLeaseConfig config{
        runtime_directory,
        ".asicen-userland-",
        group ? EndpointAccess::shared_group : EndpointAccess::private_user,
        group};
    return IdentityLease::acquire(config, identity, diagnostic);
}

inline void print_identity_lease_diagnostic(
    std::FILE* stream, const IdentityLeaseDiagnostic& diagnostic, bool group) noexcept
{
    using Reason = IdentityLeaseDiagnosticReason;
    switch (diagnostic.reason) {
    case Reason::runtime_directory:
        std::fprintf(stream, "enclosure lease: invalid --runtime-dir\n");
        return;
    case Reason::runtime_directory_missing:
        std::fprintf(stream,
                     "enclosure lease: --runtime-dir must already exist; it is not created\n");
        return;
    case Reason::runtime_directory_lookup:
        std::fprintf(stream, "enclosure lease: cannot inspect --runtime-dir\n");
        return;
    case Reason::runtime_directory_type:
        std::fprintf(stream,
                     "enclosure lease: --runtime-dir must be a real directory, not a symlink or other file type\n");
        return;
    case Reason::runtime_directory_owner:
        std::fprintf(stream,
                     "enclosure lease: --runtime-dir must be owned by the effective user\n");
        return;
    case Reason::runtime_directory_mode:
        std::fprintf(stream,
                     "enclosure lease: --runtime-dir mode check failed; expected mode %s\n",
                     group ? "0750" : "0700");
        return;
    case Reason::runtime_directory_security:
        std::fprintf(stream,
                     "enclosure lease: --runtime-dir must have a protected current-user DACL\n");
        return;
    case Reason::identity:
        std::fprintf(stream, "enclosure lease: invalid USB topology identity\n");
        return;
    case Reason::lock_prefix:
        std::fprintf(stream, "enclosure lease: invalid lock prefix\n");
        return;
    case Reason::lease_file_type:
        std::fprintf(stream,
                     "enclosure lease: lock entry must be a regular file, not a symlink or another file type\n");
        return;
    case Reason::lease_file_owner:
        std::fprintf(stream,
                     "enclosure lease: lock file must be owned by the effective user\n");
        return;
    case Reason::lease_file_mode:
        std::fprintf(stream, "enclosure lease: lock file must have mode 0600\n");
        return;
    case Reason::lease_file_links:
        std::fprintf(stream, "enclosure lease: lock file must not have aliases\n");
        return;
    case Reason::none:
        return;
    }
}

}  // namespace asicen::cli

#endif  // ASICEN_USERLAND_ASICEND_IDENTITY_LEASE_H
