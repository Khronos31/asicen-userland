// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_LOGGING_H
#define PX4_USERLAND_LOGGING_H

// Logging is provided by the MIT dtv-userland-core foundation library.
#include "dtv/userland/logging.h"

namespace px4::userland {

using dtv::userland::LogLevel;
using dtv::userland::LogRecord;
using dtv::userland::LogSink;
using dtv::userland::Logger;
using dtv::userland::log_level_string;

}  // namespace px4::userland

#endif  // PX4_USERLAND_LOGGING_H
