// SPDX-License-Identifier: GPL-2.0-or-later
#include "l3_daemon_bridge.h"

#include "asicen/card_only_service.h"
#include "daemon_backend.h"

#include "asicen/libusb_hardware_backend.h"

#include <cstdio>
#include <memory>

extern "C" {

void* asicen_l3_daemon_create(void* hardware)
{
    auto* backend = static_cast<asicen::LibusbW3u3Hardware*>(hardware);
    const auto* profile = backend == nullptr ? nullptr : backend->device_profile();
    if (profile == nullptr)
    {
        return nullptr;
    }
    auto session = asicen::l3::L3DaemonSession::create(*backend, *profile, true, nullptr);
    return session.release();
}

int asicen_l3_daemon_start(void* session)
{
    auto* daemon = static_cast<asicen::l3::L3DaemonSession*>(session);
    if (daemon->start() != DTV_L3_OK)
    {
        return 1;
    }
    return 0;
}

void asicen_l3_daemon_set_feeder(void* session, void (*feed)(void*), void* context)
{
    auto* daemon = static_cast<asicen::l3::L3DaemonSession*>(session);
    if (daemon == nullptr)
    {
        return;
    }
    daemon->stream().set_after_prepare([feed, context] {
        if (feed != nullptr)
        {
            feed(context);
        }
    });
}

int asicen_l3_daemon_run(void* session, const char* runtime_directory, const char* instance,
                         volatile sig_atomic_t* stop_requested)
{
    auto* daemon = static_cast<asicen::l3::L3DaemonSession*>(session);
    if (daemon == nullptr || daemon->backend() == nullptr)
    {
        return 70;
    }
    return asicen::run_l3_core_server(daemon->hardware(), runtime_directory, instance, false,
                                      *daemon->backend(), *daemon->backend(), stop_requested,
                                      nullptr);
}

void asicen_l3_daemon_destroy(void* session)
{
    delete static_cast<asicen::l3::L3DaemonSession*>(session);
}

} // extern "C"
