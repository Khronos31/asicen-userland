// SPDX-License-Identifier: GPL-2.0-or-later
#include "libusb_host.h"

#include "asicen/libusb_hardware_backend.h"

#include <algorithm>
#include <cstring>

namespace asicen::l3 {
namespace {

dtv_l3_result map_error(px4::userland::Error error) noexcept
{
    switch (error)
    {
    case px4::userland::Error::OK:
        return DTV_L3_OK;
    case px4::userland::Error::INVALID_ARGUMENT:
        return DTV_L3_INVALID;
    case px4::userland::Error::BUSY:
        return DTV_L3_BUSY;
    case px4::userland::Error::NOT_READY:
        return DTV_L3_BUSY;
    case px4::userland::Error::TIMEOUT:
        return DTV_L3_TIMEOUT;
    case px4::userland::Error::DISCONNECTED:
        return DTV_L3_DISCONNECTED;
    case px4::userland::Error::UNSUPPORTED:
        return DTV_L3_UNSUPPORTED;
    case px4::userland::Error::NO_CARD:
    case px4::userland::Error::CARD_REMOVED:
    case px4::userland::Error::BUFFER_TOO_SMALL:
    default:
        return DTV_L3_FAILED;
    }
}

dtv_l3_claim_result claim_failure(dtv_l3_result error) noexcept
{
    return dtv_l3_claim_result{error, DTV_L3_CLAIM_NONE};
}

dtv_l3_release_result release_failure(dtv_l3_result error) noexcept
{
    return dtv_l3_release_result{DTV_L3_RELEASE_QUIESCENCE_UNKNOWN, DTV_L3_CLAIM_RETAINED,
                                 DTV_L3_RESTORE_UNKNOWN, error};
}

std::uint32_t function_count(const LibusbW3u3Hardware& backend) noexcept
{
    const auto* profile = backend.device_profile();
    return profile == nullptr ? 0U : profile->expected_runtime_functions;
}

} // namespace

dtv_l3_claim_result LibusbBackendHost::claim(std::uint32_t claim_index)
{
    const std::uint32_t functions = function_count(backend_);
    if (functions == 0U || claim_index >= functions)
    {
        return claim_failure(DTV_L3_INVALID);
    }
    if (enclosure_claimed_)
    {
        return dtv_l3_claim_result{DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
    }
    if (claim_index != 0U)
    {
        // Function claims are served by the enclosure claim; the engine claims
        // in ascending order, so a higher index before index 0 is a programming
        // error rather than a partial physical claim.
        return claim_failure(DTV_L3_FAILED);
    }
    const auto claimed = backend_.claim();
    if (!claimed)
    {
        return claim_failure(map_error(claimed.error()));
    }
    enclosure_claimed_ = true;
    return dtv_l3_claim_result{DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
}

dtv_l3_release_result LibusbBackendHost::release_claim(std::uint32_t claim_index)
{
    const std::uint32_t functions = function_count(backend_);
    if (functions == 0U || claim_index >= functions)
    {
        return release_failure(DTV_L3_INVALID);
    }
    if (claim_index != 0U)
    {
        return dtv_l3_release_result{DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
                                     DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
    }
    if (!enclosure_claimed_)
    {
        return dtv_l3_release_result{DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
                                     DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
    }
    const auto released = backend_.release();
    if (!released)
    {
        return release_failure(map_error(released.error()));
    }
    enclosure_claimed_ = false;
    return dtv_l3_release_result{DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
                                 DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
}

dtv_l3_result LibusbBackendHost::submit(std::uint64_t, const dtv_l3_transfer_request&,
                                        dtv_l3_transfer_complete_fn, void*)
{
    // The existing backend keeps its transfer and capture plumbing private.
    // Exposing a generic submit/cancel seam is a required product/Core change.
    return DTV_L3_UNSUPPORTED;
}

dtv_l3_result LibusbBackendHost::cancel(std::uint64_t)
{
    return DTV_L3_UNSUPPORTED;
}

dtv_l3_result LibusbBackendHost::tune(std::uint32_t receiver, std::uint32_t system,
                                      std::uint64_t frequency_hz, std::uint32_t timeout_ms,
                                      std::uint64_t* programmed_hz)
{
    if (programmed_hz == nullptr || frequency_hz == 0U)
    {
        return DTV_L3_INVALID;
    }
    const auto khz = static_cast<std::uint32_t>((frequency_hz + 500U) / 1000U);
    px4::userland::Result<void> tuned =
        px4::userland::Result<void>::failure(px4::userland::Error::UNSUPPORTED);
    if (system == ASICEN_L3_SYSTEM_TERRESTRIAL)
    {
        tuned = backend_.tune_terrestrial(static_cast<std::uint8_t>(receiver), khz, timeout_ms);
    } else if (system == ASICEN_L3_SYSTEM_SATELLITE)
    {
        tuned = backend_.tune_satellite(static_cast<std::uint8_t>(receiver), khz, timeout_ms);
    } else
    {
        return DTV_L3_INVALID;
    }
    if (!tuned)
    {
        return map_error(tuned.error());
    }
    *programmed_hz = static_cast<std::uint64_t>(khz) * 1000U;
    return DTV_L3_OK;
}

dtv_l3_result LibusbBackendHost::reader_open(std::uint32_t reader_id)
{
    if (reader_id != 0U)
    {
        return DTV_L3_UNSUPPORTED;
    }
    const auto summary = backend_.probe_card(nullptr);
    return summary.detected ? DTV_L3_OK : map_error(summary.error);
}

dtv_l3_result LibusbBackendHost::reader_reset(std::uint32_t reader_id)
{
    if (reader_id != 0U)
    {
        return DTV_L3_UNSUPPORTED;
    }
    CardProbeRequest request{CardProbeMode::atr};
    const auto summary = backend_.probe_card(request, nullptr);
    return summary.atr_valid ? DTV_L3_OK : map_error(summary.error);
}

dtv_l3_result LibusbBackendHost::reader_transmit(std::uint32_t reader_id,
                                                 const std::uint8_t* command,
                                                 std::size_t command_size, std::uint8_t* response,
                                                 std::size_t response_capacity,
                                                 std::size_t* response_size)
{
    if (reader_id != 0U)
    {
        return DTV_L3_UNSUPPORTED;
    }
    if (command == nullptr || command_size == 0U || response_size == nullptr)
    {
        return DTV_L3_INVALID;
    }
    *response_size = 0U;
    CardProbeRequest request{CardProbeMode::apdu};
    request.apdu = px4::userland::ByteView{command, command_size};
    const auto summary = backend_.probe_card(request, nullptr);
    if (!summary.detected && summary.response.empty())
    {
        return map_error(summary.error);
    }
    if (summary.response.size() > response_capacity ||
        (response == nullptr && !summary.response.empty()))
    {
        return DTV_L3_FAILED;
    }
    if (!summary.response.empty())
    {
        std::memcpy(response, summary.response.data(), summary.response.size());
    }
    *response_size = summary.response.size();
    return DTV_L3_OK;
}

dtv_l3_result LibusbBackendHost::reader_close(std::uint32_t reader_id)
{
    // The card mailbox in this backend is stateless per probe; closing is a
    // no-op until a persistent card session is exposed.
    return reader_id == 0U ? DTV_L3_OK : DTV_L3_UNSUPPORTED;
}

} // namespace asicen::l3
