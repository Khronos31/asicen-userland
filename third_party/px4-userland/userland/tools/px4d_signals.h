// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_PX4D_SIGNALS_H
#define PX4_USERLAND_PX4D_SIGNALS_H

namespace px4::userland::px4d {

// Installs the platform stop handlers (POSIX signals or the Windows console
// control handler). Returns false if the platform registration failed.
bool install_signal_handlers() noexcept;

// True once a stop has been requested and the daemon should begin its
// cooperative shutdown (drain requests, LNB 0V, endpoint cleanup).
bool stop_requested() noexcept;

// Requests the same cooperative shutdown from a cooperative parent contract
// (for example, standard-input EOF on Windows) rather than a console event.
void request_stop() noexcept;

// Called by the daemon after cooperative shutdown has completed. On Windows
// this releases a blocking console/termination handler so the process can
// exit only after cleanup; on POSIX it is a no-op.
void notify_cleanup_complete() noexcept;

// True once notify_cleanup_complete() has been observed.
bool cleanup_complete() noexcept;

}  // namespace px4::userland::px4d

#endif
