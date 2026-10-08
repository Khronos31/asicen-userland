// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace asicen {

// Opens and exclusively flocks an existing regular lock file without O_CREAT.
// If absent, creates it atomically with mode 0600. The returned fd owns the
// flock; callers close it to release. The lock file itself is never removed.
int acquire_enclosure_lock(const char* path) noexcept;

}  // namespace asicen
