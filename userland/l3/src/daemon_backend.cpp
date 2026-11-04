// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/libusb_hardware_backend.h"
#include "daemon_backend.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace asicen::l3 {
namespace {

px4::userland::Error to_error(dtv_l3_result result) noexcept
{
    switch (result)
    {
    case DTV_L3_OK:
        return px4::userland::Error::OK;
    case DTV_L3_BUSY:
        return px4::userland::Error::BUSY;
    case DTV_L3_TIMEOUT:
        return px4::userland::Error::TIMEOUT;
    case DTV_L3_CANCELLED:
        return px4::userland::Error::NOT_READY;
    case DTV_L3_UNSUPPORTED:
        return px4::userland::Error::UNSUPPORTED;
    case DTV_L3_INVALID:
        return px4::userland::Error::INVALID_ARGUMENT;
    case DTV_L3_DISCONNECTED:
        return px4::userland::Error::DISCONNECTED;
    case DTV_L3_STALE:
        return px4::userland::Error::NOT_FOUND;
    default:
        return px4::userland::Error::INTERNAL;
    }
}

bool wait_owner_terminal(dtv_l3_op* op, std::uint32_t timeout_ms) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        dtv_l3_op_result result{};
        if (dtv_l3_op_poll(op, &result) != DTV_L3_OK)
        {
            return false;
        }
        if (result.state == DTV_L3_OP_DONE || result.state == DTV_L3_OP_FAILED)
        {
            return result.state == DTV_L3_OP_DONE && result.result == DTV_L3_OK;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

L3DaemonBackend::L3DaemonBackend(dtv_l3_context* owner, LibusbW3u3Hardware& hardware,
                                 const DeviceProfile& profile) noexcept
    : owner_(owner), hardware_(hardware), profile_(&profile)
{
    systems_.fill(px4::userland::ipc::System::ISDB_T);
}

L3DaemonBackend::~L3DaemonBackend()
{
    (void)shutdown();
}

std::uint8_t L3DaemonBackend::receiver_count() const noexcept
{
    return hardware_.receiver_count();
}

bool L3DaemonBackend::receiver_supports(std::uint8_t receiver,
                                        px4::userland::ipc::System system) const noexcept
{
    return hardware_.receiver_supports(receiver, system);
}

dtv_l3_lease* L3DaemonBackend::lease_for(std::uint8_t receiver) noexcept
{
    if (receiver >= leases_.size())
    {
        return nullptr;
    }
    return leases_[receiver];
}

px4::userland::Result<void> L3DaemonBackend::open_receiver(std::uint8_t receiver) noexcept
{
    if (owner_ == nullptr || receiver >= receiver_count())
    {
        return px4::userland::Result<void>::failure(px4::userland::Error::INVALID_ARGUMENT);
    }
    if (leases_[receiver] != nullptr)
    {
        return px4::userland::Result<void>::success();
    }
    const auto opened = hardware_.open_receiver(receiver);
    if (!opened)
    {
        return opened;
    }
    dtv_l3_lease* lease = nullptr;
    const auto result = dtv_l3_lease_acquire(owner_, receiver, &lease);
    if (result != DTV_L3_OK)
    {
        (void)hardware_.close_receiver(receiver);
        return px4::userland::Result<void>::failure(to_error(result));
    }
    leases_[receiver] = lease;
    return px4::userland::Result<void>::success();
}

px4::userland::Result<void> L3DaemonBackend::tune_terrestrial(std::uint8_t receiver,
                                                              std::uint32_t frequency_khz,
                                                              std::uint32_t timeout_ms) noexcept
{
    if (leases_[receiver] == nullptr)
    {
        return px4::userland::Result<void>::failure(px4::userland::Error::NOT_READY);
    }
    dtv_l3_tune_request request{};
    request.system = DTV_L3_TUNE_SYSTEM_ISDB_T;
    request.selection = DTV_L3_TUNE_SELECTION_AUTO;
    request.requested_hz = static_cast<std::uint64_t>(frequency_khz) * 1000U;
    std::uint64_t programmed = 0U;
    std::uint64_t revision = 0U;
    const auto result = dtv_l3_lease_tune_ex(leases_[receiver], &request, &programmed, &revision);
    if (result == DTV_L3_OK)
    {
        systems_[receiver] = px4::userland::ipc::System::ISDB_T;
    }
    (void)timeout_ms;
    return result == DTV_L3_OK ? px4::userland::Result<void>::success()
                               : px4::userland::Result<void>::failure(to_error(result));
}

px4::userland::Result<void> L3DaemonBackend::tune_satellite(std::uint8_t receiver,
                                                            std::uint32_t frequency_khz,
                                                            std::uint32_t timeout_ms) noexcept
{
    if (leases_[receiver] == nullptr)
    {
        return px4::userland::Result<void>::failure(px4::userland::Error::NOT_READY);
    }
    dtv_l3_tune_request request{};
    request.system = DTV_L3_TUNE_SYSTEM_ISDB_S;
    request.selection = DTV_L3_TUNE_SELECTION_AUTO;
    request.requested_hz = static_cast<std::uint64_t>(frequency_khz) * 1000U;
    std::uint64_t programmed = 0U;
    std::uint64_t revision = 0U;
    const auto result = dtv_l3_lease_tune_ex(leases_[receiver], &request, &programmed, &revision);
    if (result == DTV_L3_OK)
    {
        systems_[receiver] = px4::userland::ipc::System::ISDB_S;
    }
    (void)timeout_ms;
    return result == DTV_L3_OK ? px4::userland::Result<void>::success()
                               : px4::userland::Result<void>::failure(to_error(result));
}

px4::userland::Result<px4::userland::TunerHzProgramming>
L3DaemonBackend::tune_terrestrial_hz(std::uint8_t receiver, std::uint64_t frequency_hz,
                                     std::uint32_t timeout_ms) noexcept
{
    return hardware_.tune_terrestrial_hz(receiver, frequency_hz, timeout_ms);
}

px4::userland::Result<px4::userland::TunerHzProgramming>
L3DaemonBackend::tune_satellite_hz(std::uint8_t receiver, std::uint64_t frequency_hz,
                                   std::uint32_t timeout_ms) noexcept
{
    return hardware_.tune_satellite_hz(receiver, frequency_hz, timeout_ms);
}

px4::userland::TunerStrictProfile
L3DaemonBackend::strict_profile(std::uint8_t receiver,
                                px4::userland::ipc::System system) const noexcept
{
    return hardware_.strict_profile(receiver, system);
}

px4::userland::Result<bool> L3DaemonBackend::is_locked(std::uint8_t receiver,
                                                       px4::userland::ipc::System system) noexcept
{
    return hardware_.is_locked(receiver, system);
}

px4::userland::Result<void>
L3DaemonBackend::select_satellite_slot(std::uint8_t receiver, std::uint8_t slot,
                                       std::uint32_t timeout_ms) noexcept
{
    return hardware_.select_satellite_slot(receiver, slot, timeout_ms);
}

px4::userland::Result<void>
L3DaemonBackend::select_satellite_tsid(std::uint8_t receiver, std::uint16_t tsid,
                                       std::uint32_t timeout_ms) noexcept
{
    return hardware_.select_satellite_tsid(receiver, tsid, timeout_ms);
}

px4::userland::Result<void> L3DaemonBackend::close_receiver(std::uint8_t receiver) noexcept
{
    if (receiver >= leases_.size())
    {
        return px4::userland::Result<void>::failure(px4::userland::Error::INVALID_ARGUMENT);
    }
    for (auto it = streams_.begin(); it != streams_.end();)
    {
        if (it->second.receiver == receiver)
        {
            if (it->second.stream != nullptr)
            {
                (void)dtv_l3_stream_stop(it->second.stream, 0);
                (void)dtv_l3_stream_destroy(it->second.stream);
            }
            it = streams_.erase(it);
        } else
        {
            ++it;
        }
    }
    if (leases_[receiver] != nullptr)
    {
        (void)dtv_l3_lease_release(leases_[receiver]);
        leases_[receiver] = nullptr;
    }
    return hardware_.close_receiver(receiver);
}

px4::userland::Result<void> L3DaemonBackend::begin_tune_power(std::uint8_t receiver,
                                                              px4::userland::ipc::System system,
                                                              std::uint8_t lnb_voltage) noexcept
{
    return hardware_.begin_tune_power(receiver, system, lnb_voltage);
}

px4::userland::Result<void> L3DaemonBackend::commit_tune_power(std::uint8_t receiver) noexcept
{
    return hardware_.commit_tune_power(receiver);
}

px4::userland::Result<void> L3DaemonBackend::rollback_tune_power(std::uint8_t receiver) noexcept
{
    return hardware_.rollback_tune_power(receiver);
}

void L3DaemonBackend::mark_receiver_disconnected(std::uint8_t receiver) noexcept
{
    hardware_.mark_receiver_disconnected(receiver);
}

void L3DaemonBackend::request_stop() noexcept
{
    stop_requested_.store(true, std::memory_order_release);
    stop_all_streams();
    hardware_.request_stop();
}

px4::userland::Result<void>
L3DaemonBackend::start_capture(std::uint8_t receiver, px4::userland::ipc::System system) noexcept
{
    (void)receiver;
    (void)system;
    return px4::userland::Result<void>::success();
}

px4::userland::Result<void>
L3DaemonBackend::stop_capture(std::uint8_t receiver, px4::userland::ipc::System system) noexcept
{
    (void)receiver;
    (void)system;
    return px4::userland::Result<void>::success();
}

L3DaemonBackend::StreamEntry* L3DaemonBackend::stream_for(std::uint64_t attachment_id) noexcept
{
    const auto found = streams_.find(attachment_id);
    return found == streams_.end() ? nullptr : &found->second;
}

const L3DaemonBackend::StreamEntry*
L3DaemonBackend::stream_for(std::uint64_t attachment_id) const noexcept
{
    const auto found = streams_.find(attachment_id);
    return found == streams_.end() ? nullptr : &found->second;
}

px4::userland::Result<void>
L3DaemonBackend::attach(const px4::userland::TunerAttachment& attachment) noexcept
{
    dtv_l3_lease* lease = lease_for(attachment.receiver);
    if (lease == nullptr)
    {
        return px4::userland::Result<void>::failure(px4::userland::Error::NOT_FOUND);
    }
    auto result = dtv_l3_lease_start(lease);
    if (result != DTV_L3_OK)
    {
        return px4::userland::Result<void>::failure(to_error(result));
    }
    dtv_l3_stream* stream = nullptr;
    result = dtv_l3_lease_attach(lease, &stream);
    if (result != DTV_L3_OK)
    {
        return px4::userland::Result<void>::failure(to_error(result));
    }
    streams_[attachment.attachment_id] =
        StreamEntry{stream, attachment.receiver, attachment.system};
    return px4::userland::Result<void>::success();
}

px4::userland::Result<void>
L3DaemonBackend::detach(const px4::userland::TunerAttachment& attachment) noexcept
{
    StreamEntry* entry = stream_for(attachment.attachment_id);
    if (entry == nullptr)
    {
        return px4::userland::Result<void>::failure(px4::userland::Error::NOT_FOUND);
    }
    const auto result = dtv_l3_stream_stop(entry->stream, 1000U);
    return result == DTV_L3_OK || result == DTV_L3_TIMEOUT
               ? px4::userland::Result<void>::success()
               : px4::userland::Result<void>::failure(to_error(result));
}

px4::userland::Result<px4::userland::TunerStreamCounters>
L3DaemonBackend::stats(const px4::userland::TunerAttachment& attachment) const noexcept
{
    const StreamEntry* entry = stream_for(attachment.attachment_id);
    if (entry == nullptr)
    {
        return px4::userland::Result<px4::userland::TunerStreamCounters>::failure(
            px4::userland::Error::NOT_FOUND);
    }
    dtv_l3_stream_status status{};
    if (dtv_l3_stream_status_get(entry->stream, &status) != DTV_L3_OK)
    {
        return px4::userland::Result<px4::userland::TunerStreamCounters>::failure(
            px4::userland::Error::INTERNAL);
    }
    px4::userland::TunerStreamCounters counters{};
    counters.packets = status.queued_packets;
    counters.queue_drops = status.dropped_overflow_packets;
    counters.bytes = status.staged_bytes;
    return px4::userland::Result<px4::userland::TunerStreamCounters>::success(counters);
}

px4::userland::Result<px4::userland::TunerStreamFinalSnapshot>
L3DaemonBackend::final_snapshot(const px4::userland::TunerAttachment& attachment) const noexcept
{
    const StreamEntry* entry = stream_for(attachment.attachment_id);
    if (entry == nullptr)
    {
        return px4::userland::Result<px4::userland::TunerStreamFinalSnapshot>::failure(
            px4::userland::Error::NOT_FOUND);
    }
    auto counters_result = stats(attachment);
    if (!counters_result)
    {
        return px4::userland::Result<px4::userland::TunerStreamFinalSnapshot>::failure(
            counters_result.error());
    }
    px4::userland::TunerStreamFinalSnapshot snapshot{};
    snapshot.counters = counters_result.value();
    snapshot.terminal = static_cast<std::uint8_t>(px4::userland::TunerStreamTerminal::stopped);
    return px4::userland::Result<px4::userland::TunerStreamFinalSnapshot>::success(snapshot);
}

px4::userland::Result<void>
L3DaemonBackend::release_final(const px4::userland::TunerAttachment& attachment) noexcept
{
    const auto found = streams_.find(attachment.attachment_id);
    if (found == streams_.end())
    {
        return px4::userland::Result<void>::failure(px4::userland::Error::NOT_FOUND);
    }
    (void)dtv_l3_stream_destroy(found->second.stream);
    streams_.erase(found);
    return px4::userland::Result<void>::success();
}

px4::userland::Result<px4::userland::TunerStreamReadResult>
L3DaemonBackend::read(const px4::userland::TunerAttachment& attachment,
                      px4::userland::MutableByteView output,
                      px4::userland::Timeout timeout) noexcept
{
    const StreamEntry* entry = stream_for(attachment.attachment_id);
    if (entry == nullptr)
    {
        return px4::userland::Result<px4::userland::TunerStreamReadResult>::failure(
            px4::userland::Error::NOT_FOUND);
    }
    std::size_t bytes = 0U;
    dtv_l3_read_state state = DTV_L3_READ_WAIT;
    const auto result = dtv_l3_stream_read_ts_wait(entry->stream, output.data, output.size, &bytes,
                                                   &state, timeout.milliseconds);
    px4::userland::TunerStreamReadResult read{};
    read.bytes = bytes;
    read.eof = state == DTV_L3_READ_EOF;
    if (result == DTV_L3_WAIT_TIMEOUT)
    {
        read.timed_out = true;
        return px4::userland::Result<px4::userland::TunerStreamReadResult>::success(read);
    }
    if (result != DTV_L3_OK)
    {
        return px4::userland::Result<px4::userland::TunerStreamReadResult>::failure(
            to_error(result));
    }
    return px4::userland::Result<px4::userland::TunerStreamReadResult>::success(read);
}

px4::userland::Result<px4::userland::TunerStreamTerminal>
L3DaemonBackend::terminal(const px4::userland::TunerAttachment& attachment) const noexcept
{
    const StreamEntry* entry = stream_for(attachment.attachment_id);
    if (entry == nullptr)
    {
        return px4::userland::Result<px4::userland::TunerStreamTerminal>::failure(
            px4::userland::Error::NOT_FOUND);
    }
    dtv_l3_stream_status status{};
    if (dtv_l3_stream_status_get(entry->stream, &status) != DTV_L3_OK)
    {
        return px4::userland::Result<px4::userland::TunerStreamTerminal>::failure(
            px4::userland::Error::INTERNAL);
    }
    if (status.phase == DTV_L3_STREAM_RETIRED)
    {
        return px4::userland::Result<px4::userland::TunerStreamTerminal>::success(
            px4::userland::TunerStreamTerminal::stopped);
    }
    return px4::userland::Result<px4::userland::TunerStreamTerminal>::success(
        px4::userland::TunerStreamTerminal::none);
}

void L3DaemonBackend::stop_all_streams() noexcept
{
    for (auto& entry : streams_)
    {
        if (entry.second.stream != nullptr)
        {
            (void)dtv_l3_stream_stop(entry.second.stream, 0);
        }
    }
}

void L3DaemonBackend::release_all_leases() noexcept
{
    for (auto& lease : leases_)
    {
        if (lease != nullptr)
        {
            (void)dtv_l3_lease_release(lease);
            lease = nullptr;
        }
    }
}

px4::userland::Result<void> L3DaemonBackend::shutdown() noexcept
{
    stop_all_streams();
    for (auto& entry : streams_)
    {
        if (entry.second.stream != nullptr)
        {
            (void)dtv_l3_stream_destroy(entry.second.stream);
        }
    }
    streams_.clear();
    release_all_leases();
    return px4::userland::Result<void>::success();
}

L3DaemonSession::L3DaemonSession(LibusbW3u3Hardware& hardware, const DeviceProfile& profile,
                                 bool fake_claim)
    : hardware_(hardware), profile_(&profile), fake_claim_(fake_claim), host_(hardware),
      stream_(hardware)
{
    if (!fake_claim_)
    {
        real_provider_ = std::make_unique<LibusbUsbProvider>(hardware_, &stream_);
        provider_ = *real_provider_->provider();
    } else
    {
        provider_.provider_context = this;
        provider_.claim = &L3DaemonSession::fake_claim;
        provider_.release_claim = &L3DaemonSession::fake_release;
        provider_.submit = &L3DaemonSession::fake_submit;
        provider_.cancel = &L3DaemonSession::fake_cancel;
        provider_.cancel_scope = DTV_L3_CANCEL_SCOPE_TRANSFER;
    }
    factory_ = std::make_unique<Factory>(*profile_, host_, &stream_);
}

L3DaemonSession::~L3DaemonSession()
{
    shutdown();
}

dtv_l3_claim_result L3DaemonSession::fake_claim(void*, dtv_l3_provider_binding*, std::uint32_t)
{
    return {DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
}

dtv_l3_release_result L3DaemonSession::fake_release(void*, dtv_l3_provider_binding*, std::uint32_t)
{
    return {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE, DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
}

dtv_l3_result L3DaemonSession::fake_submit(void* context, dtv_l3_provider_binding* binding,
                                           std::uint64_t id, const dtv_l3_transfer_request* request,
                                           dtv_l3_transfer_complete_fn complete,
                                           void* callback_context)
{
    auto* session = static_cast<L3DaemonSession*>(context);
    return session->stream_.submit(binding, id, request, complete, callback_context);
}

dtv_l3_result L3DaemonSession::fake_cancel(void* context, dtv_l3_provider_binding*,
                                           std::uint64_t id)
{
    auto* session = static_cast<L3DaemonSession*>(context);
    return session->stream_.cancel(id);
}

std::unique_ptr<L3DaemonSession> L3DaemonSession::create(LibusbW3u3Hardware& hardware,
                                                         const DeviceProfile& profile,
                                                         bool fake_claim, dtv_l3_firmware* firmware)
{
    auto session =
        std::unique_ptr<L3DaemonSession>(new L3DaemonSession(hardware, profile, fake_claim));
    session->firmware_ = firmware;
    return session;
}

dtv_l3_result L3DaemonSession::start()
{
    if (owner_ != nullptr)
    {
        return DTV_L3_OK;
    }
    dtv_l3_open_config config{};
    config.model = static_cast<std::uint32_t>(profile_->model_id);
    config.selector = 0U;
    config.firmware = firmware_;
    const auto opened =
        dtv_l3_open_factory(&provider_, factory_->core_factory(), &config, factory_->claim_count());
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == nullptr || opened.op == nullptr)
    {
        return opened.result;
    }
    if (!wait_owner_terminal(opened.op, 5000U))
    {
        (void)dtv_l3_op_release(opened.op);
        return DTV_L3_FAILED;
    }
    (void)dtv_l3_op_release(opened.op);
    dtv_l3_open_status status{};
    if (dtv_l3_open_status_get(opened.owner, &status) != DTV_L3_OK ||
        status.state != DTV_L3_OPEN_READY)
    {
        return DTV_L3_FAILED;
    }
    owner_ = opened.owner;
    backend_ = std::make_unique<L3DaemonBackend>(owner_, hardware_, *profile_);
    return DTV_L3_OK;
}

void L3DaemonSession::shutdown()
{
    if (backend_ != nullptr)
    {
        (void)backend_->shutdown();
        backend_.reset();
    }
    if (owner_ != nullptr)
    {
        (void)hardware_.shutdown();
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const auto closed = dtv_l3_close(owner_, 1000U);
            if (closed == DTV_L3_OK)
            {
                break;
            }
            (void)dtv_l3_cleanup_retry(owner_);
        }
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            dtv_l3_close_status status{};
            if (dtv_l3_close_status_get(owner_, &status) == DTV_L3_OK && status.releasable != 0U)
            {
                break;
            }
            (void)dtv_l3_cleanup_retry(owner_);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        (void)dtv_l3_context_release(owner_);
        owner_ = nullptr;
    }
    if (!fake_claim_)
    {
        (void)hardware_.release();
    }
}

} // namespace asicen::l3
