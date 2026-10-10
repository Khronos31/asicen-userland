#include "asicen/enclosure.h"

namespace asicen {

px4::userland::Result<ReceiverAddress> receiver_address(const DeviceProfile& profile,
                                                        std::size_t receiver) noexcept
{
    using px4::userland::Error;
    using px4::userland::Result;
    if (!profile_runtime_supported(profile) || receiver >= profile.enclosure_receiver_count) {
        return Result<ReceiverAddress>::failure(Error::INVALID_ARGUMENT);
    }

    // Public receiver numbering is grouped by runtime function so the mapping
    // remains stable even before physical connector labels are confirmed.
    ReceiverAddress address{};
    address.function_index = static_cast<std::uint8_t>(receiver / 2U);
    address.local_lane = static_cast<std::uint8_t>(receiver % 2U);
    address.system = w3u3_system_for_local_lane(address.local_lane);
    // V2 TF_AssignDevExt makes role0 the shared RF master. Its public source
    // sequence S0,T0,S1,T1 is the internal T0,S0,T1,S1 sequence with bit0 flipped.
    // Other families have independent RF control on each stream-owning function.
    address.rf_function_index =
        profile.frontend_family == FrontendFamily::Nmi ? 0U : address.function_index;
    address.frontend_source = profile.frontend_family == FrontendFamily::Nmi
                                  ? static_cast<std::uint8_t>(receiver ^ 1U)
                                  : address.local_lane;
    return Result<ReceiverAddress>::success(address);
}

}  // namespace asicen
