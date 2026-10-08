// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "px4/identity.h"

#include <string>

namespace px4::userland::tools {

// Formats `px4d --list` (SPEC 4.6).  Existing records and fields remain in
// place; USB locations, serial uniqueness and receiver LNB capabilities are
// appended.  USB devices of other models are not printed.
std::string format_device_list(const GroupingResult& grouping);

// Formats `px4d --list-json` as one compact JSON object followed by newline.
std::string format_device_list_json(const GroupingResult& grouping);

} // namespace px4::userland::tools
