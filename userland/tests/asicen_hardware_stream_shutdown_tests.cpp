// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_stream_session.h"
#include "asicen/px4_mock_backend.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {
using namespace px4::userland;

class BlockingSource final : public asicen::StreamCaptureSource {
public:
    Result<void> prepare(std::uint8_t receiver, ipc::System system,
                         const std::atomic<bool>& cancelled) noexcept override
    {
        prepared.store(receiver == 0U && system == ipc::System::ISDB_T && !cancelled.load(),
                       std::memory_order_release);
        return prepared.load(std::memory_order_acquire) ? Result<void>::success()
                                                       : Result<void>::failure(Error::UNSUPPORTED);
    }
    asicen::CaptureRunResult run(const std::atomic<bool>& cancelled,
                                 bool (*)(void*, const std::uint8_t*, std::size_t),
                                 void*) noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex);
        run_entered.store(true, std::memory_order_release);
        condition.notify_all();
        condition.wait(lock, [&] {
            return cancelled.load(std::memory_order_acquire) || interrupted;
        });
        run_returning.store(true, std::memory_order_release);
        return asicen::CaptureRunResult::cancelled;
    }
    void interrupt() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            interrupted = true;
            interrupt_seen.store(true, std::memory_order_release);
        }
        condition.notify_all();
    }
    Result<void> stop() noexcept override
    {
        stop_seen.store(true, std::memory_order_release);
        return Result<void>::success();
    }
    std::atomic<bool> prepared{false};
    std::atomic<bool> run_entered{false};
    std::atomic<bool> run_returning{false};
    std::atomic<bool> interrupt_seen{false};
    std::atomic<bool> stop_seen{false};
private:
    std::mutex mutex;
    std::condition_variable condition;
    bool interrupted = false;
};

void observe_read_wait(void* context) noexcept
{
    static_cast<std::atomic<bool>*>(context)->store(true, std::memory_order_release);
}

template <typename Predicate>
bool wait_for(std::chrono::milliseconds timeout, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

bool test_real_hardware_stream_read_stops_at_source()
{
    asicen::MockTunerBackend frontend(1U);
    BlockingSource source;
    std::atomic<bool> read_wait_entered{false};
    asicen::HardwareStreamService stream(frontend, source,
        asicen::HardwareStreamService::TestQueueCapacityBytes{188U}, nullptr,
        &observe_read_wait, &read_wait_entered);
    if (!stream.start_capture(0U, ipc::System::ISDB_T)) return false;
    TunerAttachment attachment{};
    attachment.owner_client_id = 11U;
    attachment.lease_id = 12U;
    attachment.attachment_id = 13U;
    attachment.receiver = 0U;
    attachment.system = ipc::System::ISDB_T;
    attachment.nonce[0] = 0x5aU;
    if (!stream.attach(attachment)) return false;
    std::array<std::uint8_t, 188U> buffer{};
    std::atomic<bool> read_done{false};
    std::atomic<bool> read_ok{false};
    std::atomic<bool> read_eof{false};
    std::atomic<std::size_t> read_bytes{1U};
    std::thread reader([&] {
        const auto result = stream.read(attachment, MutableByteView{buffer.data(), buffer.size()},
                                        Timeout{30000U});
        if (result) {
            read_ok.store(true, std::memory_order_release);
            read_eof.store(result.value().eof, std::memory_order_release);
            read_bytes.store(result.value().bytes, std::memory_order_release);
        }
        read_done.store(true, std::memory_order_release);
    });
    const bool entered = wait_for(std::chrono::seconds(2), [&] {
        return source.run_entered.load(std::memory_order_acquire) &&
               read_wait_entered.load(std::memory_order_acquire);
    });
    if (!entered) {
        stream.request_stop();
        reader.join();
        (void)stream.shutdown();
        return false;
    }
    const bool read_waiting_before_stop =
        read_wait_entered.load(std::memory_order_acquire) &&
        !read_done.load(std::memory_order_acquire) &&
        source.run_entered.load(std::memory_order_acquire);
    const auto start = std::chrono::steady_clock::now();
    stream.request_stop();
    const bool returned = wait_for(std::chrono::seconds(2), [&] {
        return source.run_returning.load(std::memory_order_acquire) &&
               read_done.load(std::memory_order_acquire);
    });
    if (!returned) {
        stream.request_stop();
        (void)stream.shutdown();
    }
    reader.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto stopped = stream.shutdown();
    const bool ok = read_waiting_before_stop && returned && stopped &&
        elapsed < std::chrono::seconds(2) &&
        source.prepared.load() && source.interrupt_seen.load() &&
        source.run_returning.load() && source.stop_seen.load() && read_ok.load() &&
        read_eof.load() && read_bytes.load() == 0U;
    if (!ok) std::fprintf(stderr, "real HardwareStreamService blocked-read shutdown proof failed\n");
    return ok;
}
}  // namespace

int main()
{
    return test_real_hardware_stream_read_stops_at_source() ? 0 : 1;
}
