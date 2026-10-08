#pragma once

#include "asicen/stream_capture.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace asicen {

enum class QueueWait : std::uint8_t { Completion, Timeout, Error };
enum class FilterRepeat : std::uint8_t { None, BeforeQueue, AfterPostStartBit };

struct QueueCompletion {
    std::size_t slot = 0;
    CaptureIo io = CaptureIo::Error;
    const unsigned char* data = nullptr;  // borrowed until resubmit/release
    std::size_t size = 0;
    std::uint64_t generation = 0;
    int raw_status = -1;
    int requested_length = 0;
    int actual_length = 0;
};

enum class QueuePhase : std::uint8_t { Normal, StoppingDsc, CancelDrain };

struct QueueCallbackEvent {
    std::size_t slot = 0;
    std::uint64_t generation = 0;
    std::uint64_t monotonic_ns = 0;
    QueuePhase phase = QueuePhase::Normal;
    int raw_status = -1;
    int requested_length = 0;
    int actual_length = 0;
};

struct QueueCancelEvent {
    std::size_t slot = 0;
    std::uint64_t generation = 0;
    int return_code = 0;
};

struct QueueObservation {
    static constexpr std::size_t kMaxEvents = 256;
    static constexpr std::size_t kMaxSlots = 4;
    static constexpr std::size_t kStatusCount = 8;

    std::array<QueueCallbackEvent, kMaxEvents> events{};
    std::size_t event_count = 0;
    std::uint64_t event_overflow = 0;
    std::uint64_t callback_count = 0;
    std::uint64_t callback_actual_bytes = 0;
    std::array<std::uint64_t, 3> phase_counts{};
    std::array<std::uint64_t, 3> phase_actual_bytes{};
    std::array<std::uint64_t, kStatusCount> status_counts{};
    std::array<std::uint64_t, kStatusCount> status_actual_bytes{};
    std::uint64_t unknown_status_count = 0;
    std::uint64_t normal_delivery_count = 0;
    std::uint64_t normal_delivery_actual_bytes = 0;
    std::size_t pending_before_stop = 0;
    std::size_t ready_before_stop = 0;
    std::array<QueueCancelEvent, kMaxSlots> cancellations{};
    std::size_t cancellation_count = 0;
    std::uint64_t cancellation_overflow = 0;
    std::uint64_t duplicate_or_stale_callbacks = 0;

    void set_phase(QueuePhase phase);
    void record_callback(std::size_t slot, std::uint64_t generation,
                         int raw_status, int requested_length, int actual_length);
    void record_normal_delivery(std::size_t size);
    void record_before_stop(std::size_t pending, std::size_t ready);
    void record_cancel(std::size_t slot, std::uint64_t generation, int return_code);

private:
    QueuePhase phase_ = QueuePhase::Normal;
    std::array<std::uint64_t, kMaxSlots> last_generation_{};
    std::array<bool, kMaxSlots> saw_generation_{};
};

// Owns async-transfer buffers and callbacks until cancel_and_drain returns.
// Implementations must not release callback state while an event is pending.
class QueuedCaptureIo {
public:
    virtual ~QueuedCaptureIo() = default;
    virtual bool prepare(std::uint8_t endpoint, std::size_t depth,
                         std::size_t chunk_size) = 0;
    virtual bool submit(std::size_t slot) = 0;
    virtual QueueWait wait(unsigned timeout_ms, QueueCompletion* completion) = 0;
    virtual bool resubmit(std::size_t slot) = 0;
    virtual void set_observation(QueueObservation* observation) {
        (void)observation;
    }
    virtual void set_phase(QueuePhase phase) { (void)phase; }
    virtual void snapshot_before_stop() {}
    virtual void cancel_and_drain() = 0;
    virtual void release() = 0;
};

CaptureOutcome run_queued_capture(CaptureBackend* control, QueuedCaptureIo* io,
                                 CaptureOutput* output,
                                 const CaptureRequest& request,
                                 std::size_t depth, CaptureStats* stats,
                                 bool filter_start = false,
                                 const std::uint8_t* original_cf40 = nullptr,
                                 QueueObservation* observation = nullptr,
                                 FilterRepeat filter_repeat = FilterRepeat::None,
                                 std::uint8_t reset_state = 1,
                                 const std::uint8_t* original_cf_block = nullptr,
                                 bool link_seed = false);

}  // namespace asicen
