// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_L3_DAEMON_BACKEND_H
#define ASICEN_USERLAND_L3_DAEMON_BACKEND_H

#include "libusb_host.h"

#include "asicen/device_profile.h"

#include "px4/tuner_service.h"

#include <array>
#include <atomic>
#include <map>
#include <memory>

namespace asicen::l3 {

// Product-internal bridge between the existing px4 TunerService/IPC assembly
// and the Core L3 owner. It keeps the same physical hardware/claim instance as
// the card and frontend components; it is not a new public API.
class L3DaemonBackend final : public px4::userland::TunerServiceBackend,
                              public px4::userland::TunerStreamControl {
  public:
    L3DaemonBackend(dtv_l3_context* owner, LibusbW3u3Hardware& hardware,
                    const DeviceProfile& profile) noexcept;
    ~L3DaemonBackend() override;

    std::uint8_t receiver_count() const noexcept override;
    bool receiver_supports(std::uint8_t receiver,
                           px4::userland::ipc::System system) const noexcept override;
    px4::userland::Result<void> open_receiver(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> tune_terrestrial(std::uint8_t receiver, std::uint32_t frequency_khz,
                                                 std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> tune_satellite(std::uint8_t receiver, std::uint32_t frequency_khz,
                                               std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<px4::userland::TunerHzProgramming>
    tune_terrestrial_hz(std::uint8_t receiver, std::uint64_t frequency_hz,
                        std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<px4::userland::TunerHzProgramming>
    tune_satellite_hz(std::uint8_t receiver, std::uint64_t frequency_hz,
                      std::uint32_t timeout_ms) noexcept override;
    px4::userland::TunerStrictProfile
    strict_profile(std::uint8_t receiver,
                   px4::userland::ipc::System system) const noexcept override;
    px4::userland::Result<bool> is_locked(std::uint8_t receiver,
                                          px4::userland::ipc::System system) noexcept override;
    px4::userland::Result<void> select_satellite_slot(std::uint8_t receiver, std::uint8_t slot,
                                                      std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> select_satellite_tsid(std::uint8_t receiver, std::uint16_t tsid,
                                                      std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> close_receiver(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> begin_tune_power(std::uint8_t receiver,
                                                 px4::userland::ipc::System system,
                                                 std::uint8_t lnb_voltage) noexcept override;
    px4::userland::Result<void> commit_tune_power(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> rollback_tune_power(std::uint8_t receiver) noexcept override;
    void mark_receiver_disconnected(std::uint8_t receiver) noexcept override;
    void request_stop() noexcept override;
    px4::userland::Result<void> shutdown() noexcept override;
    px4::userland::Result<void> start_capture(std::uint8_t receiver,
                                              px4::userland::ipc::System system) noexcept override;
    px4::userland::Result<void> stop_capture(std::uint8_t receiver,
                                             px4::userland::ipc::System system) noexcept override;

    px4::userland::Result<void>
    attach(const px4::userland::TunerAttachment& attachment) noexcept override;
    px4::userland::Result<void>
    detach(const px4::userland::TunerAttachment& attachment) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamCounters>
    stats(const px4::userland::TunerAttachment& attachment) const noexcept override;
    px4::userland::Result<px4::userland::TunerStreamFinalSnapshot>
    final_snapshot(const px4::userland::TunerAttachment& attachment) const noexcept override;
    px4::userland::Result<void>
    release_final(const px4::userland::TunerAttachment& attachment) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamReadResult>
    read(const px4::userland::TunerAttachment& attachment, px4::userland::MutableByteView output,
         px4::userland::Timeout timeout) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamTerminal>
    terminal(const px4::userland::TunerAttachment& attachment) const noexcept override;

  private:
    struct StreamEntry {
        dtv_l3_stream* stream = nullptr;
        std::uint8_t receiver = 0U;
        px4::userland::ipc::System system = px4::userland::ipc::System::ISDB_T;
    };

    dtv_l3_lease* lease_for(std::uint8_t receiver) noexcept;
    StreamEntry* stream_for(std::uint64_t attachment_id) noexcept;
    const StreamEntry* stream_for(std::uint64_t attachment_id) const noexcept;
    void stop_all_streams() noexcept;
    void release_all_leases() noexcept;

    dtv_l3_context* owner_ = nullptr;
    LibusbW3u3Hardware& hardware_;
    const DeviceProfile* profile_ = nullptr;
    std::array<dtv_l3_lease*, 4> leases_{};
    std::map<std::uint64_t, StreamEntry> streams_;
    std::array<px4::userland::ipc::System, 4> systems_{};
    std::atomic<bool> stop_requested_{false};
};

class L3DaemonSession final {
  public:
    static std::unique_ptr<L3DaemonSession> create(LibusbW3u3Hardware& hardware,
                                                   const DeviceProfile& profile, bool fake_claim,
                                                   dtv_l3_firmware* firmware);
    ~L3DaemonSession();

    dtv_l3_result start();
    void shutdown();
    L3DaemonBackend* backend() noexcept
    {
        return backend_.get();
    }
    dtv_l3_context* owner() const noexcept
    {
        return owner_;
    }
    LibusbStreamSubmit& stream() noexcept
    {
        return stream_;
    }
    LibusbW3u3Hardware& hardware() noexcept
    {
        return hardware_;
    }

  private:
    L3DaemonSession(LibusbW3u3Hardware& hardware, const DeviceProfile& profile, bool fake_claim);

    static dtv_l3_claim_result fake_claim(void*, dtv_l3_provider_binding*, std::uint32_t);
    static dtv_l3_release_result fake_release(void*, dtv_l3_provider_binding*, std::uint32_t);
    static dtv_l3_result fake_submit(void* context, dtv_l3_provider_binding* binding,
                                     std::uint64_t id, const dtv_l3_transfer_request* request,
                                     dtv_l3_transfer_complete_fn complete, void* callback_context);
    static dtv_l3_result fake_cancel(void* context, dtv_l3_provider_binding*, std::uint64_t id);

    LibusbW3u3Hardware& hardware_;
    const DeviceProfile* profile_ = nullptr;
    dtv_l3_firmware* firmware_ = nullptr;
    bool fake_claim_ = false;
    LibusbProductHost host_;
    LibusbStreamSubmit stream_;
    std::unique_ptr<LibusbUsbProvider> real_provider_;
    dtv_l3_usb_provider provider_{};
    std::unique_ptr<Factory> factory_;
    dtv_l3_context* owner_ = nullptr;
    std::unique_ptr<L3DaemonBackend> backend_;
};

} // namespace asicen::l3

#endif
