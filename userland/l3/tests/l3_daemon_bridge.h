// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_L3_DAEMON_BRIDGE_H
#define ASICEN_L3_DAEMON_BRIDGE_H

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

void* asicen_l3_daemon_create(void* hardware);
int asicen_l3_daemon_start(void* session);
void asicen_l3_daemon_set_feeder(void* session, void (*feed)(void*), void* context);
int asicen_l3_daemon_run(void* session, const char* runtime_directory, const char* instance,
                         volatile sig_atomic_t* stop_requested);
void asicen_l3_daemon_destroy(void* session);

#ifdef __cplusplus
}
#endif

#endif
