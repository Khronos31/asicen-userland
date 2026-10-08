// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/px4_mock_backend.h"

#include "asicen/product_profile.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace asicen {
using namespace px4::userland;

namespace {

using namespace px4::userland;

Result<void> available(const std::atomic<bool>& stopping) noexcept
{
    return stopping.load() ? Result<void>::failure(Error::NOT_READY)
                           : Result<void>::success();
}

}  // namespace

std::uint8_t MockTunerBackend::receiver_count() const noexcept
{
    return profile::kReceiverCount;
}

bool MockTunerBackend::receiver_supports(std::uint8_t receiver,
                                         ipc::System system) const noexcept
{
    if (receiver >= profile::kReceiverCount) return false;
    return system == (profile::is_satellite_receiver(receiver)
                          ? ipc::System::ISDB_S : ipc::System::ISDB_T);
}

Result<void> MockTunerBackend::open_receiver(std::uint8_t receiver) noexcept
{
    if (receiver >= profile::kReceiverCount) return Result<void>::failure(Error::NOT_FOUND);
    return available(stopping_);
}

Result<void> MockTunerBackend::tune_terrestrial(std::uint8_t receiver,
    std::uint32_t frequency_khz, std::uint32_t timeout_ms) noexcept
{
    if (!receiver_supports(receiver, ipc::System::ISDB_T) ||
        frequency_khz < 40000U || frequency_khz > 1002000U || timeout_ms < 100U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return available(stopping_);
}

Result<void> MockTunerBackend::tune_satellite(std::uint8_t receiver,
    std::uint32_t frequency_khz, std::uint32_t timeout_ms) noexcept
{
    if (!receiver_supports(receiver, ipc::System::ISDB_S) ||
        frequency_khz < 146875U || frequency_khz > 2350000U || timeout_ms < 100U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return available(stopping_);
}

Result<bool> MockTunerBackend::is_locked(std::uint8_t receiver,
                                         ipc::System system) noexcept
{
    if (!receiver_supports(receiver, system)) return Result<bool>::failure(Error::INVALID_ARGUMENT);
    if (stopping_.load()) return Result<bool>::failure(Error::NOT_READY);
    return Result<bool>::success(true);
}

Result<void> MockTunerBackend::select_satellite_slot(std::uint8_t receiver,
    std::uint8_t slot, std::uint32_t) noexcept
{
    if (!receiver_supports(receiver, ipc::System::ISDB_S) || slot >= 12U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return available(stopping_);
}

Result<void> MockTunerBackend::select_satellite_tsid(std::uint8_t receiver,
    std::uint16_t tsid, std::uint32_t) noexcept
{
    if (!receiver_supports(receiver, ipc::System::ISDB_S) || tsid == 0xffffU)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return available(stopping_);
}

Result<void> MockTunerBackend::close_receiver(std::uint8_t receiver) noexcept
{
    if (receiver >= profile::kReceiverCount) return Result<void>::failure(Error::NOT_FOUND);
    return Result<void>::success();
}

Result<void> MockTunerBackend::start_capture(std::uint8_t receiver,
                                             ipc::System system) noexcept
{
    if (!receiver_supports(receiver, system)) return Result<void>::failure(Error::UNSUPPORTED);
    return available(stopping_);
}

Result<void> MockTunerBackend::stop_capture(std::uint8_t receiver,
                                             ipc::System system) noexcept
{
    if (!receiver_supports(receiver, system)) return Result<void>::failure(Error::UNSUPPORTED);
    return Result<void>::success();
}

Result<void> MockTunerBackend::shutdown() noexcept
{
    stopping_.store(true);
    return Result<void>::success();
}

void MockTunerBackend::request_stop() noexcept
{
    stopping_.store(true);
}

Result<void> MockTunerStream::attach(const TunerAttachment& attachment) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (streams_.find(attachment.attachment_id) != streams_.end())
        return Result<void>::failure(Error::BUSY);
    State state;
    state.identity = attachment;
    streams_.emplace(attachment.attachment_id, state);
    return Result<void>::success();
}

Result<void> MockTunerStream::detach(const TunerAttachment& attachment) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = streams_.find(attachment.attachment_id);
    if (found == streams_.end()) return Result<void>::failure(Error::NOT_FOUND);
    found->second.active = false;
    found->second.detached = true;
    found->second.final = TunerStreamFinalSnapshot{
        found->second.counters,
        static_cast<std::uint8_t>(TunerStreamTerminal::stopped)};
    return Result<void>::success();
}

Result<TunerStreamCounters> MockTunerStream::stats(
    const TunerAttachment& attachment) const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = streams_.find(attachment.attachment_id);
    if (found == streams_.end()) return Result<TunerStreamCounters>::failure(Error::NOT_FOUND);
    return Result<TunerStreamCounters>::success(found->second.counters);
}

Result<TunerStreamFinalSnapshot> MockTunerStream::final_snapshot(
    const TunerAttachment& attachment) const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = streams_.find(attachment.attachment_id);
    if (found == streams_.end())
        return Result<TunerStreamFinalSnapshot>::failure(Error::NOT_FOUND);
    if (found->second.detached) return Result<TunerStreamFinalSnapshot>::success(found->second.final);
    return Result<TunerStreamFinalSnapshot>::success(
        TunerStreamFinalSnapshot{found->second.counters,
                                 static_cast<std::uint8_t>(TunerStreamTerminal::none)});
}

Result<void> MockTunerStream::release_final(const TunerAttachment& attachment) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = streams_.find(attachment.attachment_id);
    if (found == streams_.end()) return Result<void>::failure(Error::NOT_FOUND);
    if (!found->second.detached) return Result<void>::failure(Error::NOT_READY);
    streams_.erase(found);
    return Result<void>::success();
}

Result<TunerStreamReadResult> MockTunerStream::read(
    const TunerAttachment& attachment, MutableByteView output, Timeout) noexcept
{
    if (output.data == nullptr || output.size < 188U)
        return Result<TunerStreamReadResult>::failure(Error::BUFFER_TOO_SMALL);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = streams_.find(attachment.attachment_id);
    if (found == streams_.end()) return Result<TunerStreamReadResult>::failure(Error::NOT_FOUND);
    if (!found->second.active) {
        return Result<TunerStreamReadResult>::success(
            TunerStreamReadResult{0U, true, false, TunerStreamTerminal::stopped});
    }
    const std::size_t packet_count = std::min<std::size_t>(output.size / 188U, 64U);
    for (std::size_t packet = 0U; packet < packet_count; ++packet) {
        std::uint8_t* bytes = output.data + packet * 188U;
        std::memset(bytes, 0xff, 188U);
        bytes[0] = 0x47U;
        bytes[1] = 0x1fU;
        bytes[2] = 0xffU;
        bytes[3] = static_cast<std::uint8_t>(0x10U |
            (found->second.counters.packets & 0x0fU));
        ++found->second.counters.packets;
    }
    const std::size_t byte_count = packet_count * 188U;
    found->second.counters.bytes += byte_count;
    return Result<TunerStreamReadResult>::success(
        TunerStreamReadResult{byte_count, false, false, TunerStreamTerminal::none});
}

Result<TunerStreamTerminal> MockTunerStream::terminal(
    const TunerAttachment& attachment) const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = streams_.find(attachment.attachment_id);
    if (found == streams_.end()) return Result<TunerStreamTerminal>::failure(Error::NOT_FOUND);
    return Result<TunerStreamTerminal>::success(found->second.active
        ? TunerStreamTerminal::none : TunerStreamTerminal::stopped);
}

Result<void> UnsupportedCardBackend::set_power(bool) noexcept
{ return Result<void>::success(); }

Result<void> UnsupportedCardBackend::initialize_uart() noexcept
{ return Result<void>::success(); }

Result<bool> UnsupportedCardBackend::detect_card() noexcept
{ return Result<bool>::failure(Error::UNSUPPORTED); }

Result<void> UnsupportedCardSession::initialize() noexcept
{ return Result<void>::failure(px4::userland::Error::UNSUPPORTED); }

Result<std::size_t> UnsupportedCardSession::transmit(ByteView, MutableByteView) noexcept
{ return Result<std::size_t>::failure(px4::userland::Error::UNSUPPORTED); }

bool UnsupportedCardSession::initialized() const noexcept { return false; }

const CardAtr& UnsupportedCardSession::atr() const noexcept { return atr_; }

void UnsupportedCardSession::invalidate() noexcept { atr_ = CardAtr{}; }

}  // namespace asicen
