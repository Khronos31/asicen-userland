#pragma once

#include "asicen/stream_capture.h"

#include <cstddef>
#include <cstdint>

namespace asicen {

enum class QueueWait : std::uint8_t { Completion, Timeout, Error };

struct QueueCompletion {
    std::size_t slot = 0;
    CaptureIo io = CaptureIo::Error;
    const unsigned char* data = nullptr;  // borrowed until resubmit/release
    std::size_t size = 0;
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
    virtual void cancel_and_drain() = 0;
    virtual void release() = 0;
};

CaptureOutcome run_queued_capture(CaptureBackend* control, QueuedCaptureIo* io,
                                 CaptureOutput* output,
                                 const CaptureRequest& request,
                                 std::size_t depth, CaptureStats* stats,
                                 bool filter_start = false,
                                 const std::uint8_t* original_cf40 = nullptr);

}  // namespace asicen
