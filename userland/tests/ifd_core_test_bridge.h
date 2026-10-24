// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_IFD_CORE_TEST_BRIDGE_H
#define ASICEN_IFD_CORE_TEST_BRIDGE_H

#include <cstdint>

extern "C" {

void* asicen_ifd_test_create(const char* device_name) noexcept;
int asicen_ifd_test_presence(void* session) noexcept;
int asicen_ifd_test_power(void* session, bool power_up) noexcept;
int asicen_ifd_test_interface_status(void* session, std::uint8_t* status) noexcept;
int asicen_ifd_test_close(void* session) noexcept;

}

#endif
