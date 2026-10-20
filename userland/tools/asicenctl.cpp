// SPDX-License-Identifier: GPL-2.0-only
#include "px4/identity.h"

#include "host_ctl.h"

#if defined(_WIN32)
#include "px4_windows_args.h"
#endif

#include <cstring>
#include <string_view>

#if defined(_WIN32)
#include <string>
#include <vector>
#endif

namespace {

bool preserve_instance(std::string_view value,
                       dtv::userland::identity::InstanceText& output) noexcept
{
    if (!px4::userland::valid_runtime_instance(value) ||
        value.size() > output.bytes.size())
        return false;
    output.length = static_cast<std::uint16_t>(value.size());
    std::memcpy(output.bytes.data(), value.data(), value.size());
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    dtv::userland::host_ctl::Profile profile{};
    profile.tool_name = "asicenctl";
    profile.product_slug = "asicen-userland";
    profile.product_version = ASICEN_PRODUCT_VERSION;
    profile.product = dtv::userland::identity::Product::asicen;
    profile.card_supported = true;
    profile.serial_selector_supported = false;
    profile.normalize_instance = &preserve_instance;
#if defined(_WIN32)
    const std::vector<std::string> owned_argv =
        px4::userland::cli::windows_argv_utf8(argc, argv);
    std::vector<const char*> argv_views;
    argv_views.reserve(owned_argv.size());
    for (const std::string& value : owned_argv) argv_views.push_back(value.c_str());
    return dtv::userland::host_ctl::run_control_cli(
        static_cast<int>(argv_views.size()), argv_views.data(), profile);
#else
    return dtv::userland::host_ctl::run_control_cli(argc, argv, profile);
#endif
}
