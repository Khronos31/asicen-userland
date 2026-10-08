// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "px4/ipc.h"

#include <cstddef>
#include <string>

namespace px4::userland::tools {

// Payloads passed here have already crossed the IPC decoder boundary.  In
// particular, decode_status_response_payload() has validated ReceiverState.
std::string format_list(const ipc::ListResponsePayload& payload);
std::string format_status(const ipc::StatusResponsePayload& payload);
// One "receiver=... device=... local=... system=..." line per record.  Shared
// by `px4ctl list` and `px4d --list` so both print receivers identically.
std::string format_receiver_records(const ipc::ReceiverRecord* records, std::size_t count);

} // namespace px4::userland::tools
