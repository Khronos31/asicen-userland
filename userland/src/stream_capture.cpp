#include "asicen/stream_capture.h"

#include <algorithm>
#include <ostream>
#include <vector>

namespace asicen {
namespace {

int remaining_ms(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
}

}  // namespace

CaptureOutcome run_raw_capture(CaptureBackend* backend, CaptureOutput* output,
                               const CaptureRequest& request, CaptureStats* stats) {
    if (backend == nullptr || output == nullptr || request.endpoint == 0 ||
        request.local > 1 || request.chunk_size == 0) {
        return CaptureOutcome::InvalidArgument;
    }

    if (!backend->dsc_start(request.local)) {
        return CaptureOutcome::StartFailed;
    }

    bool output_failed = false;
    bool usb_failed = false;
    bool limit_reached = false;
    bool cancelled = false;
    std::uint64_t total = 0;
    std::vector<unsigned char> buffer(request.chunk_size);

    while (true) {
        if (backend->cancelled()) {
            cancelled = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= request.deadline) {
            break;
        }
        if (request.byte_limit != 0 && total >= request.byte_limit) {
            limit_reached = true;
            break;
        }

        int transferred = 0;
        const int timeout = std::min(1000, std::max(1, remaining_ms(request.deadline)));
        const CaptureIo io =
            backend->bulk_read(request.endpoint, buffer.data(),
                               static_cast<int>(buffer.size()), &transferred,
                               static_cast<unsigned>(timeout));

        // Always account for transferred bytes before classifying completion.
        if (transferred > 0) {
            std::size_t to_write = static_cast<std::size_t>(transferred);
            if (request.byte_limit != 0 && total + to_write > request.byte_limit) {
                to_write = static_cast<std::size_t>(request.byte_limit - total);
            }
            if (!output->write(buffer.data(), to_write)) {
                output_failed = true;
                break;
            }
            total += to_write;
        }

        if (io == CaptureIo::Error) {
            usb_failed = true;
            break;
        }
        // Classify completion only after an explicit error check, so an error
        // carrying bytes at the exact limit cannot be hidden.
        if (request.byte_limit != 0 && total >= request.byte_limit) {
            limit_reached = true;
            break;
        }
        // Ok or Timeout (possibly partial) continue until deadline/cancel.
    }

    const bool stop_ok = backend->dsc_stop(request.local);
    if (stats != nullptr) {
        stats->bytes = total;
        stats->limit_reached = limit_reached;
    }

    if (!stop_ok) {
        return CaptureOutcome::StopFailed;
    }
    if (output_failed) {
        return CaptureOutcome::OutputFailed;
    }
    if (usb_failed) {
        return CaptureOutcome::UsbFailed;
    }
    if (cancelled) {
        return CaptureOutcome::Cancelled;
    }
    if (total == 0) {
        return CaptureOutcome::ZeroBytes;
    }
    if (request.byte_limit != 0 && !limit_reached) {
        return CaptureOutcome::LimitNotReached;
    }
    return CaptureOutcome::Completed;
}

const char* capture_outcome_name(CaptureOutcome outcome) {
    switch (outcome) {
        case CaptureOutcome::Completed:
            return "completed";
        case CaptureOutcome::ZeroBytes:
            return "zero-bytes";
        case CaptureOutcome::LimitNotReached:
            return "limit-not-reached";
        case CaptureOutcome::OutputFailed:
            return "output-failed";
        case CaptureOutcome::UsbFailed:
            return "usb-failed";
        case CaptureOutcome::StartFailed:
            return "start-failed";
        case CaptureOutcome::StopFailed:
            return "stop-failed";
        case CaptureOutcome::Cancelled:
            return "cancelled";
        case CaptureOutcome::InvalidArgument:
            return "invalid-argument";
    }
    return "unknown";
}

void write_command_summary(std::ostream& normal, std::ostream& diagnostic,
                           bool capture_command, const char* model,
                           const char* port, std::uint8_t local) {
    std::ostream& output = capture_command ? diagnostic : normal;
    output << "model=\"" << model << "\" port=" << port << " local="
           << static_cast<unsigned>(local) << '\n';
}

}  // namespace asicen
