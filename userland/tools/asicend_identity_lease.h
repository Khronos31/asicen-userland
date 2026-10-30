// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_ASICEND_IDENTITY_LEASE_H
#define ASICEN_USERLAND_ASICEND_IDENTITY_LEASE_H

#include "dtv/userland/host_listener_context/identity_lease.h"

#include <climits>
#include <cstdio>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace asicen::cli {

using dtv::userland::Error;
using dtv::userland::Result;
using dtv::userland::host_listener_context::IdentityLease;
using dtv::userland::host_listener_context::IdentityLeaseConfig;
using dtv::userland::host_listener_context::IdentityLeaseDiagnostic;
using dtv::userland::host_listener_context::IdentityLeaseDiagnosticReason;
using dtv::userland::host_runtime::EndpointAccess;

inline bool local_app_data_utf8(std::string& output) noexcept
{
#if defined(_WIN32)
    const DWORD required = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0U);
    if (required == 0U) return false;
    std::wstring wide(required, L'\0');
    const DWORD length = ::GetEnvironmentVariableW(
        L"LOCALAPPDATA", wide.data(), required);
    if (length == 0U || length >= required) return false;
    wide.resize(length);
    if (wide.size() > static_cast<std::size_t>(INT_MAX)) return false;
    const int bytes = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return false;
    output.resize(static_cast<std::size_t>(bytes));
    return ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        output.data(), bytes, nullptr, nullptr) == bytes;
#else
    (void)output;
    return false;
#endif
}

inline Result<IdentityLease> acquire_identity_lease(
    std::string_view runtime_directory, bool group, std::string_view identity,
    IdentityLeaseDiagnostic* diagnostic = nullptr) noexcept
{
#if defined(_WIN32)
    if (group) return Result<IdentityLease>::failure(Error::UNSUPPORTED);
    std::string local_app_data;
    if (runtime_directory.empty()) {
        if (!local_app_data_utf8(local_app_data))
            return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
        runtime_directory = local_app_data;
    }
#endif
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
