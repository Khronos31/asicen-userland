// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_MOCK_TRANSPORT_H
#define PX4_USERLAND_MOCK_TRANSPORT_H

#include "px4/transport.h"

// MockTransport behavior lives in the MIT dtv-userland-core foundation
// library.  The product keeps a default-constructible facade that supplies
// the PX4 device policy explicitly.
#include "dtv/userland/mock_transport.h"

namespace px4::userland {

using dtv::userland::MockOperation;
using dtv::userland::MockOutcome;
using dtv::userland::MockTransportPolicy;

class MockTransport final : public dtv::userland::MockTransport {
public:
    MockTransport() noexcept
        : dtv::userland::MockTransport(
              MockTransportPolicy{kMaxCommandTransfer, kMaxStreamTransfer, kTsInEndpoint})
    {
    }

    ~MockTransport() noexcept override = default;
};

}  // namespace px4::userland

#endif  // PX4_USERLAND_MOCK_TRANSPORT_H
