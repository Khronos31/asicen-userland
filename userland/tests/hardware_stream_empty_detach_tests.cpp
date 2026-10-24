// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_stream_session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

using namespace px4::userland;

class Backend final : public TunerServiceBackend {
public:
    std::uint8_t receiver_count() const noexcept override { return 8U; }
    bool receiver_supports(std::uint8_t receiver, ipc::System system) const noexcept override
    { return receiver == 2U && system == ipc::System::ISDB_T; }
    Result<void> open_receiver(std::uint8_t) noexcept override
    { return Result<void>::success(); }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<bool> is_locked(std::uint8_t, ipc::System) noexcept override
    { return Result<bool>::success(true); }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> close_receiver(std::uint8_t) noexcept override
    { return Result<void>::success(); }
};

class EmptySource final : public asicen::StreamCaptureSource {
public:
    Result<void> prepare(std::uint8_t, ipc::System,
                         const std::atomic<bool>&) noexcept override
    { return Result<void>::success(); }

    asicen::CaptureRunResult run(const std::atomic<bool>& cancelled,
                                 bool (*)(void*, const std::uint8_t*, std::size_t),
                                 void*) noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        started_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] { return cancelled.load() || interrupted_; });
        return asicen::CaptureRunResult::cancelled;
    }

    void interrupt() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            interrupted_ = true;
        }
        condition_.notify_all();
    }

    Result<void> stop() noexcept override { return Result<void>::success(); }

    bool wait_started()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(1), [&] { return started_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool started_ = false;
    bool interrupted_ = false;
};

} // namespace

int main()
{
    Backend backend;
    EmptySource source;
    asicen::HardwareStreamService service(backend, source);
    TunerAttachment attachment{};
    attachment.owner_client_id = 1U;
    attachment.lease_id = 2U;
    attachment.attachment_id = 3U;
    attachment.receiver = 2U;
    attachment.system = ipc::System::ISDB_T;
    const auto started = service.start_capture(2U, ipc::System::ISDB_T);
    if (!started) {
        std::cerr << "FAIL: start empty stream error="
                  << static_cast<int>(started.error()) << "\n";
        return 1;
    }
    if (!service.attach(attachment)) {
        std::cerr << "FAIL: attach empty stream\n";
        return 1;
    }
    if (!source.wait_started()) {
        std::cerr << "FAIL: empty source worker did not start\n";
        return 1;
    }
    if (!service.detach(attachment)) {
        std::cerr << "FAIL: detach empty stream\n";
        return 1;
    }
    std::uint8_t output[188U]{};
    const auto read = service.read(attachment, {output, sizeof(output)}, {0U});
    if (!read || read.value().bytes != 0U || !read.value().eof ||
        read.value().terminal != TunerStreamTerminal::stopped) {
        std::cerr << "FAIL: empty detach must return stopped EOF to the final attachment\n";
        return 1;
    }
    if (!service.stop_capture(2U, ipc::System::ISDB_T) || !service.release_final(attachment)) {
        std::cerr << "FAIL: empty stream cleanup\n";
        return 1;
    }
    return 0;
}
