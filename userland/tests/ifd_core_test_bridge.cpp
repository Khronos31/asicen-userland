// SPDX-License-Identifier: GPL-2.0-or-later
#include "ifd_core_test_bridge.h"

#include "dtv/userland/unix_pcsc_ifd/pcsc_ifd_adapter.h"

namespace {
using namespace dtv::userland::unix_pcsc_ifd;

struct Session final {
    PosixIfdCardClientFactory factory;
    IfdAdapter adapter;

    Session() : factory(kAsicenIfdProfile), adapter(factory, kAsicenIfdProfile) {}
};
}

extern "C" void* asicen_ifd_test_create(const char* device_name) noexcept
{
    Session* session = new Session;
    if (session->adapter.create_channel_by_name(0U, device_name) != IfdResult::success) {
        delete session;
        return nullptr;
    }
    return session;
}

extern "C" int asicen_ifd_test_presence(void* opaque) noexcept
{
    if (opaque == nullptr) return -1;
    const IfdResult result = static_cast<Session*>(opaque)->adapter.presence(0U);
    if (result == IfdResult::icc_present) return 1;
    if (result == IfdResult::icc_not_present) return 0;
    return -1;
}

extern "C" int asicen_ifd_test_power(void* opaque, bool power_up) noexcept
{
    if (opaque == nullptr) return -1;
    std::uint8_t atr[kIfdAtrMaxLength]{};
    std::size_t atr_length = sizeof(atr);
    const IfdPowerAction action = power_up ? IfdPowerAction::power_up
                                          : IfdPowerAction::power_down;
    return static_cast<int>(static_cast<Session*>(opaque)->adapter.power(
        0U, action, dtv::userland::MutableByteView{atr, sizeof(atr)}, atr_length));
}

extern "C" int asicen_ifd_test_interface_status(void* opaque, std::uint8_t* status) noexcept
{
    if (opaque == nullptr || status == nullptr) return -1;
    std::size_t length = sizeof(*status);
    return static_cast<int>(static_cast<Session*>(opaque)->adapter.get_capability(
        0U, kAttrIccInterfaceStatus,
        dtv::userland::MutableByteView{status, sizeof(*status)}, length));
}

extern "C" int asicen_ifd_test_close(void* opaque) noexcept
{
    if (opaque == nullptr) return -1;
    Session* session = static_cast<Session*>(opaque);
    const IfdResult result = session->adapter.close_channel(0U);
    delete session;
    return static_cast<int>(result);
}
