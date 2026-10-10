// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/enclosure.h"

#include <cstdio>

int main()
{
    for (std::size_t model = 0U; model < asicen::profile_count(); ++model) {
        const auto& profile = asicen::profiles()[model];
        for (std::size_t receiver = 0U; receiver < profile.enclosure_receiver_count; ++receiver) {
            const auto result = asicen::receiver_address(profile, receiver);
            if (!result) {
                return 1;
            }
            const auto& route = result.value();
            const bool v2 = profile.frontend_family == asicen::FrontendFamily::Nmi;
            if (route.function_index != receiver / 2U || route.local_lane != receiver % 2U ||
                route.rf_function_index != (v2 ? 0U : receiver / 2U) ||
                route.frontend_source != (v2 ? receiver ^ 1U : receiver % 2U) ||
                route.system != (receiver % 2U == 0U ? asicen::BroadcastSystem::IsdbS
                                                     : asicen::BroadcastSystem::IsdbT)) {
                std::fprintf(stderr, "receiver route mismatch: model=%s receiver=%zu\n",
                             profile.model, receiver);
                return 1;
            }
        }
        if (asicen::receiver_address(profile, profile.enclosure_receiver_count).error() !=
            px4::userland::Error::INVALID_ARGUMENT) {
            return 1;
        }
    }
    return 0;
}
