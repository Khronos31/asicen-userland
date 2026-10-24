// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_ASICEND_SELECTOR_H
#define ASICEN_USERLAND_ASICEND_SELECTOR_H

#include "asicend_identity.h"
#include "asicen/loader_firmware.h"
#include <dtv/userland/identity.h>
#include "px4/error.h"

#include <algorithm>
#include <string>
#include <vector>

namespace asicen::cli {

struct SelectedEnclosure final {
    const DeviceProfile* model = nullptr;
    std::vector<UsbFunctionObservation> functions;
};

inline bool known_dual_function_pair(const UsbFunctionObservation& first,
                                     const UsbFunctionObservation& second) noexcept
{
    if (first.bus != second.bus || !first.parent_hub_known || !second.parent_hub_known ||
        first.parent_hub_vid != 0x05e3U || first.parent_hub_pid != 0x0608U ||
        second.parent_hub_vid != 0x05e3U || second.parent_hub_pid != 0x0608U ||
        first.port_path.size() < 2U || first.port_path.size() != second.port_path.size() ||
        !std::equal(first.port_path.begin(), first.port_path.end() - 1,
                    second.port_path.begin())) {
        return false;
    }
    return (first.port_path.back() == 1U && second.port_path.back() == 2U) ||
           (first.port_path.back() == 2U && second.port_path.back() == 1U);
}

inline px4::userland::Result<SelectedEnclosure> select_enclosure(
    const dtv::userland::identity::Selector& selector,
    const std::vector<UsbFunctionObservation>& observations,
    const DeviceProfile* requested_model, bool allow_sibling_restore)
{
    using px4::userland::Error;
    using dtv::userland::identity::SelectorKind;
    if (selector.kind == SelectorKind::serial) {
        return px4::userland::Result<SelectedEnclosure>::failure(Error::NOT_FOUND);
    }
    if (selector.topology.count == 0U || selector.topology.count > 2U) {
        return px4::userland::Result<SelectedEnclosure>::failure(Error::INVALID_ARGUMENT);
    }

    std::vector<UsbFunctionObservation> selected;
    for (std::size_t path_index = 0U; path_index < selector.topology.count; ++path_index) {
        const auto& path = selector.topology.paths[path_index];
        const std::string wanted(path.bytes.data(), path.length);
        const UsbFunctionObservation* match = nullptr;
        std::size_t matches = 0U;
        for (const auto& observation : observations) {
            if (port_string(observation) == wanted) {
                match = &observation;
                ++matches;
            }
        }
        if (matches != 1U || match == nullptr) {
            return px4::userland::Result<SelectedEnclosure>::failure(
                matches == 0U ? Error::NOT_FOUND : Error::NOT_READY);
        }
        const auto* profile = find_profile(match->vid, match->pid);
        const bool loader = match->vid == 0x1738U &&
            (match->pid == 0x5211U || match->pid == 0x5216U);
        if (profile == nullptr && !loader) {
            return px4::userland::Result<SelectedEnclosure>::failure(Error::UNSUPPORTED);
        }
        if (requested_model != nullptr && profile != nullptr &&
            profile->model_id != requested_model->model_id) {
            return px4::userland::Result<SelectedEnclosure>::failure(Error::UNSUPPORTED);
        }
        selected.push_back(*match);
    }

    const auto model_for = [](const UsbFunctionObservation& function) {
        return find_profile(function.vid, function.pid);
    };
    const auto is_loader = [](const UsbFunctionObservation& function) {
        return function.vid == 0x1738U &&
            (function.pid == 0x5211U || function.pid == 0x5216U);
    };
    const auto compatible_functions = [&](const UsbFunctionObservation& first,
                                          const UsbFunctionObservation& second) {
        if (!known_dual_function_pair(first, second)) return false;
        const auto* first_model = model_for(first);
        const auto* second_model = model_for(second);
        if (first_model != nullptr && first_model->expected_runtime_functions != 2U)
            return false;
        if (second_model != nullptr && second_model->expected_runtime_functions != 2U)
            return false;
        if (first_model != nullptr && second_model != nullptr &&
            first_model->model_id != second_model->model_id) return false;
        if (first_model != nullptr && second_model != nullptr) return true;
        if (is_loader(first) && is_loader(second) && first.vid == second.vid &&
            first.pid == second.pid) return true;
        return (first_model != nullptr && is_loader(second)) ||
               (second_model != nullptr && is_loader(first));
    };

    const DeviceProfile* inferred_model = nullptr;
    for (const auto& function : selected) {
        const auto* profile = model_for(function);
        if (profile == nullptr) continue;
        if (inferred_model != nullptr && inferred_model->model_id != profile->model_id) {
            return px4::userland::Result<SelectedEnclosure>::failure(Error::UNSUPPORTED);
        }
        inferred_model = profile;
    }

    if (selected.size() == 2U) {
        if (!compatible_functions(selected[0], selected[1])) {
            return px4::userland::Result<SelectedEnclosure>::failure(Error::UNSUPPORTED);
        }
    } else {
        const auto& first = selected.front();
        const auto* profile = model_for(first);
        bool needs_sibling = profile != nullptr
            ? profile->expected_runtime_functions == 2U
            : (requested_model != nullptr && requested_model->expected_runtime_functions == 2U) ||
                  is_loader(first);
        if (needs_sibling) {
            std::size_t sibling_count = 0U;
            UsbFunctionObservation sibling{};
            for (const auto& candidate : observations) {
                if (&candidate == &first || !compatible_functions(first, candidate)) continue;
                if (is_loader(first) &&
                    (!is_loader(candidate) || first.vid != candidate.vid ||
                     first.pid != candidate.pid)) continue;
                if (profile != nullptr) {
                    const auto* candidate_model = model_for(candidate);
                    if (candidate_model == nullptr ||
                        candidate_model->model_id != profile->model_id) continue;
                }
                sibling = candidate;
                ++sibling_count;
            }
            if (sibling_count != 1U) {
                if (sibling_count == 0U && is_loader(first) && allow_sibling_restore &&
                    first.parent_hub_known && first.parent_hub_vid == 0x05e3U &&
                    first.parent_hub_pid == 0x0608U && first.port_path.size() >= 2U &&
                    (first.port_path.back() == 1U || first.port_path.back() == 2U)) {
                    sibling = first;
                    sibling.port_path.back() = first.port_path.back() == 1U ? 2U : 1U;
                    sibling.address = 0U;
                    sibling_count = 1U;
                } else if (sibling_count == 0U && is_loader(first) &&
                           requested_model == nullptr) {
                    needs_sibling = false;
                }
            }
            if (needs_sibling && sibling_count != 1U) {
                return px4::userland::Result<SelectedEnclosure>::failure(
                    sibling_count == 0U ? Error::NOT_FOUND : Error::NOT_READY);
            }
            if (needs_sibling) selected.push_back(sibling);
        }
    }

    if (requested_model != nullptr && inferred_model != nullptr &&
        requested_model->model_id != inferred_model->model_id) {
        return px4::userland::Result<SelectedEnclosure>::failure(Error::UNSUPPORTED);
    }
    if (inferred_model == nullptr) inferred_model = requested_model;
    if (selected.size() == 2U && selected[0].port_path.back() == 2U)
        std::swap(selected[0], selected[1]);
    SelectedEnclosure result;
    result.model = inferred_model;
    result.functions = std::move(selected);
    return px4::userland::Result<SelectedEnclosure>::success(std::move(result));
}

}  // namespace asicen::cli

#endif  // ASICEN_USERLAND_ASICEND_SELECTOR_H
