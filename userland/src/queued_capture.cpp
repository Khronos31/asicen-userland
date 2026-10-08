#include "asicen/queued_capture.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace asicen {
namespace {

int remaining_ms(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - now)
                                .count());
}

}  // namespace

CaptureOutcome run_queued_capture(CaptureBackend* control, QueuedCaptureIo* io,
                                 CaptureOutput* output,
                                 const CaptureRequest& request,
                                 std::size_t depth, CaptureStats* stats) {
    if (stats != nullptr) *stats = {};
    if (control == nullptr || io == nullptr || output == nullptr ||
        request.endpoint == 0 || request.local > 1 || request.chunk_size == 0 ||
        depth == 0 || depth > 4) {
        return CaptureOutcome::InvalidArgument;
    }

    if (!io->prepare(request.endpoint, depth, request.chunk_size)) {
        io->release();
        return CaptureOutcome::UsbFailed;
    }

    std::vector<bool> pending(depth, false);
    bool setup_failed = false;
    for (std::size_t slot = 0; slot < depth; ++slot) {
        if (!io->submit(slot)) {
            setup_failed = true;
            break;
        }
        pending[slot] = true;
    }

    bool dsc_attempted = false;
    bool dsc_started = false;
    bool dsc_stopped = false;
    bool output_failed = false;
    bool usb_failed = setup_failed;
    bool limit_reached = false;
    bool cancelled = false;
    std::uint64_t total = 0;

    if (!setup_failed) {
        dsc_attempted = true;
        dsc_started = control->dsc_start(request.local);
        if (!dsc_started) usb_failed = true;
    }

    while (dsc_started) {
        if (control->cancelled()) {
            cancelled = true;
            break;
        }
        const int left = remaining_ms(request.deadline);
        if (left <= 0) break;
        if (request.byte_limit != 0 && total >= request.byte_limit) {
            limit_reached = true;
            break;
        }

        QueueCompletion completion{};
        const QueueWait waited = io->wait(
            static_cast<unsigned>(std::min(1000, std::max(1, left))), &completion);
        if (waited == QueueWait::Timeout) continue;
        if (waited == QueueWait::Error) {
            usb_failed = true;
            break;
        }
        if (completion.slot >= depth || !pending[completion.slot] ||
            (completion.size != 0 && completion.data == nullptr)) {
            usb_failed = true;
            break;
        }

        pending[completion.slot] = false;
        std::size_t to_write = completion.size;
        if (request.byte_limit != 0 && total + to_write > request.byte_limit) {
            to_write = static_cast<std::size_t>(request.byte_limit - total);
        }
        if (to_write != 0 && !output->write(completion.data, to_write)) {
            output_failed = true;
            break;
        }
        total += to_write;

        if (completion.io == CaptureIo::Error) {
            usb_failed = true;
            break;
        }
        if (request.byte_limit != 0 && total >= request.byte_limit) {
            limit_reached = true;
            break;
        }
        if (control->cancelled()) {
            cancelled = true;
            break;
        }
        if (remaining_ms(request.deadline) <= 0) break;
        if (!io->resubmit(completion.slot)) {
            usb_failed = true;
            break;
        }
        pending[completion.slot] = true;
    }

    // Stop the device-side stream before cancelling host URBs. A failed start
    // is also stopped because the control transfer may have partially taken effect.
    if (dsc_attempted) {
        dsc_stopped = control->dsc_stop(request.local);
    }
    io->cancel_and_drain();
    io->release();

    if (stats != nullptr) {
        stats->bytes = total;
        stats->limit_reached = limit_reached;
    }
    if (dsc_attempted && !dsc_stopped) return CaptureOutcome::StopFailed;
    if (output_failed) return CaptureOutcome::OutputFailed;
    if (usb_failed) return CaptureOutcome::UsbFailed;
    if (cancelled) return CaptureOutcome::Cancelled;
    if (total == 0) return CaptureOutcome::ZeroBytes;
    if (request.byte_limit != 0 && !limit_reached)
        return CaptureOutcome::LimitNotReached;
    return CaptureOutcome::Completed;
}

}  // namespace asicen
