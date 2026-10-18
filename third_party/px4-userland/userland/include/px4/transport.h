// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_TRANSPORT_H
#define PX4_USERLAND_TRANSPORT_H

#include "px4/error.h"

// The neutral byte-view/transport types live in the MIT dtv-userland-core
// foundation library.  The product keeps its device endpoint/transfer
// constants here and aliases the shared types into px4::userland.
#include "dtv/userland/transport.h"

#include <cstddef>
#include <cstdint>

namespace px4::userland {

inline constexpr std::size_t kMaxCommandTransfer = 65535U;
inline constexpr std::size_t kMaxStreamTransfer = 1048576U;
inline constexpr std::uint8_t kCommandInEndpoint = 0x81U;
inline constexpr std::uint8_t kCommandOutEndpoint = 0x02U;
inline constexpr std::uint8_t kTsInEndpoint = 0x84U;
inline constexpr std::uint8_t kObservedTsInEndpoint = 0x85U;

using dtv::userland::BulkReadObservation;
using dtv::userland::ByteView;
using dtv::userland::MutableByteView;
using dtv::userland::StreamConfig;
using dtv::userland::StreamEvent;
using dtv::userland::StreamEventKind;
using dtv::userland::Timeout;
using dtv::userland::Transport;

}  // namespace px4::userland

#endif  // PX4_USERLAND_TRANSPORT_H
