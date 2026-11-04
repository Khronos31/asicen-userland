// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_L3_LIBUSB_HOST_H
#define ASICEN_USERLAND_L3_LIBUSB_HOST_H

#include "adapter.h"

#include <cstdint>

namespace asicen {

class LibusbW3u3Hardware;

}

namespace asicen::l3 {

// Adapts the existing enclosure backend to the candidate L3 host seam. The
// current backend claims the enclosure atomically, so only claim index 0
// performs a physical claim; higher logical function indices are reported as
// already covered by that enclosure claim.
class LibusbBackendHost final : public Host {
  public:
    explicit LibusbBackendHost(LibusbW3u3Hardware& backend) : backend_(backend)
    {
    }

    dtv_l3_claim_result claim(std::uint32_t claim_index) override;
    dtv_l3_release_result release_claim(std::uint32_t claim_index) override;
    dtv_l3_result submit(std::uint64_t transfer_id, const dtv_l3_transfer_request& request,
                         dtv_l3_transfer_complete_fn complete, void* callback_context) override;
    dtv_l3_result cancel(std::uint64_t transfer_id) override;
    dtv_l3_result tune(std::uint32_t receiver, std::uint32_t system, std::uint64_t frequency_hz,
                       std::uint32_t timeout_ms, std::uint64_t* programmed_hz) override;
    dtv_l3_result reader_open(std::uint32_t reader_id) override;
    dtv_l3_result reader_reset(std::uint32_t reader_id) override;
    dtv_l3_result reader_transmit(std::uint32_t reader_id, const std::uint8_t* command,
                                  std::size_t command_size, std::uint8_t* response,
                                  std::size_t response_capacity,
                                  std::size_t* response_size) override;
    dtv_l3_result reader_close(std::uint32_t reader_id) override;

  private:
    LibusbW3u3Hardware& backend_;
    bool enclosure_claimed_ = false;
};

} // namespace asicen::l3

#endif // ASICEN_USERLAND_L3_LIBUSB_HOST_H
