// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_ASICEND_RUNTIME_CONTEXT_H
#define ASICEN_USERLAND_ASICEND_RUNTIME_CONTEXT_H

#include "asicen/libusb_transport.h"

namespace asicen::cli {

inline int initialize_fresh_context(asicen::LibusbContext& context)
{
    context.close();
    return context.initialize(false);
}

}  // namespace asicen::cli

#endif  // ASICEN_USERLAND_ASICEND_RUNTIME_CONTEXT_H
