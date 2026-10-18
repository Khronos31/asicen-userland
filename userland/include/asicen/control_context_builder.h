// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_CONTROL_CONTEXT_BUILDER_H
#define ASICEN_USERLAND_CONTROL_CONTEXT_BUILDER_H

#include "asicen/device_profile.h"
#include "asicen/usb_function_observation.h"
#include "px4/control_server.h"

namespace asicen {

bool build_asicen_control_context(
    const DeviceProfile& profile, const ClaimedUsbFunctionObservation* observations,
    std::uint8_t function_count, std::uint8_t receiver_count,
    const px4::userland::TunerService& tuner, const char* instance,
    const dtv::userland::wire::Epoch& epoch,
    px4::userland::ipc::posix::ControlContext& out,
    bool tuner_enabled = true) noexcept;

bool build_asicen_mock_control_context(
    const DeviceProfile& profile, const px4::userland::TunerService& tuner,
    const char* instance, const dtv::userland::wire::Epoch& epoch,
    px4::userland::ipc::posix::ControlContext& out) noexcept;

}  // namespace asicen

#endif  // ASICEN_USERLAND_CONTROL_CONTEXT_BUILDER_H
