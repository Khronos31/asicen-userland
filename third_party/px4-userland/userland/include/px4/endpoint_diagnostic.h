// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PX4_USERLAND_ENDPOINT_DIAGNOSTIC_H
#define PX4_USERLAND_ENDPOINT_DIAGNOSTIC_H

#include "posix_ipc.h"

#include <cstdio>

namespace asicen::cli {

inline void print_endpoint_diagnostic(
    std::FILE* stream, const px4::userland::ipc::posix::EndpointDiagnostic& diagnostic,
    bool shared_group) noexcept
{
    using Reason = px4::userland::ipc::posix::EndpointDiagnosticReason;
    switch (diagnostic.reason) {
    case Reason::path_too_long:
        std::fprintf(stream,
            "enclosure endpoint: path is too long (%zu UTF-8 bytes; %zu bytes including NUL required, native sun_path capacity %zu bytes including NUL); shorten --runtime-dir or --instance consistently for daemon and clients\n",
            diagnostic.path_bytes, diagnostic.required_bytes,
            diagnostic.native_capacity);
        return;
    case Reason::runtime_directory_type:
        std::fprintf(stream,
            "enclosure endpoint: --runtime-dir must resolve to a real directory, not a symlink or other file type\n");
        return;
    case Reason::runtime_directory_owner:
        std::fprintf(stream,
            "enclosure endpoint: --runtime-dir owner check failed; it must be owned by the effective user (or the configured shared group)\n");
        return;
    case Reason::runtime_directory_mode:
        if (shared_group) {
            std::fprintf(stream,
                "enclosure endpoint: --runtime-dir mode check failed; expected mode 0770 for the configured shared group\n");
        } else {
            std::fprintf(stream,
                "enclosure endpoint: --runtime-dir mode check failed; expected mode 0700\n");
        }
        return;
    case Reason::runtime_directory_security:
        std::fprintf(stream,
            "enclosure endpoint: --runtime-dir security check failed; require current-user ownership and a protected DACL granting access only to the current user\n");
        return;
    case Reason::lease_file_type:
        std::fprintf(stream,
            "enclosure endpoint: runtime lease check failed; the lock entry must be a regular file, not a symlink or another file type\n");
        return;
    case Reason::lease_file_owner:
        std::fprintf(stream,
            "enclosure endpoint: runtime lease owner check failed; the lease file must be owned by the effective user\n");
        return;
    case Reason::lease_file_mode:
        std::fprintf(stream,
            "enclosure endpoint: runtime lease mode check failed; the lease file must have mode 0600\n");
        return;
    case Reason::lease_file_links:
        std::fprintf(stream,
            "enclosure endpoint: runtime lease link-count check failed; the lock file must not have aliases\n");
        return;
    case Reason::instance:
        std::fprintf(stream,
            "enclosure endpoint: invalid --instance; use a nonempty path component containing only ASCII letters, digits, '.', '_' or '-'\n");
        return;
    case Reason::endpoint_name:
        std::fprintf(stream,
            "enclosure endpoint: invalid endpoint name in runtime configuration\n");
        return;
    case Reason::runtime_directory:
        std::fprintf(stream,
            "enclosure endpoint: invalid --runtime-dir; use an absolute local runtime path\n");
        return;
    case Reason::none:
        break;
    }
}

}  // namespace asicen::cli

#endif  // PX4_USERLAND_ENDPOINT_DIAGNOSTIC_H
