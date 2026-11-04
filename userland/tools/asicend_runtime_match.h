// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_ASICEND_RUNTIME_MATCH_H
#define ASICEN_USERLAND_ASICEND_RUNTIME_MATCH_H

#include "asicen/device_profile.h"
#include "asicen/enclosure_grouping.h"
#include "asicen/loader_firmware.h"

#include <cstdint>
#include <vector>

namespace asicen::cli {

enum class RuntimeCandidateStatus : std::uint8_t {
    OtherBus,
    OtherPath,
    UnknownProfile,
    UnsupportedProfile,
    MissingManifest,
    WrongModel,
    Match,
};

inline RuntimeCandidateStatus classify_runtime_candidate(
    const UsbFunctionObservation& observation, std::uint8_t bus,
    const std::vector<std::uint8_t>& port_path,
    const DeviceProfile* expected_model) noexcept
{
    if (observation.bus != bus) return RuntimeCandidateStatus::OtherBus;
    if (observation.port_path != port_path) return RuntimeCandidateStatus::OtherPath;
    const auto* profile = find_profile(observation.vid, observation.pid);
    if (profile == nullptr) return RuntimeCandidateStatus::UnknownProfile;
    if (!profile_runtime_supported(*profile)) return RuntimeCandidateStatus::UnsupportedProfile;
    if (loader_firmware_manifest(profile->model_id) == nullptr)
        return RuntimeCandidateStatus::MissingManifest;
    if (expected_model != nullptr && profile->model_id != expected_model->model_id)
        return RuntimeCandidateStatus::WrongModel;
    return RuntimeCandidateStatus::Match;
}

}  // namespace asicen::cli

#endif  // ASICEN_USERLAND_ASICEND_RUNTIME_MATCH_H
