// SPDX-License-Identifier: GPL-2.0-only
#include "host_ctl.h"

#include <algorithm>
#include <string_view>

namespace {

bool normalize_instance(std::string_view input,
                        dtv::userland::identity::InstanceText& output) noexcept
{
    if ((input.size() == 14U || input.size() == 15U) &&
        std::all_of(input.begin(), input.end(), [](char value) {
            return value >= '0' && value <= '9';
        }))
        return false;
    const auto normalized = dtv::userland::identity::canonicalize_instance(input);
    if (!normalized) return false;
    output = normalized.value();
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    const dtv::userland::host_ctl::Profile profile{
        "asicenctl", "asicen-userland", ASICEN_PROJECT_VERSION,
        dtv::userland::identity::Product::asicen, true, false,
        &normalize_instance, nullptr, false, false};
    return dtv::userland::host_ctl::run_control_cli(
        argc, const_cast<const char* const*>(argv), profile);
}
