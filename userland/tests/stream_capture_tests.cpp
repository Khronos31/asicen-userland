#include "asicen/stream_capture.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void check(bool value, const char* name) {
    if (!value) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

using Clock = std::chrono::steady_clock;

class FakeBackend final : public asicen::CaptureBackend {
public:
    bool dsc_start(std::uint8_t) override { return start_ok; }
    bool dsc_stop(std::uint8_t) override { return stop_ok; }
    asicen::CaptureIo bulk_read(std::uint8_t, unsigned char* data, int length,
                                int* transferred, unsigned) override {
        if (read_index >= io.size()) {
            *transferred = 0;
            return asicen::CaptureIo::Timeout;
        }
        int bytes = transfers[read_index];
        if (bytes > length) {
            bytes = length;
        }
        for (int i = 0; i < bytes; ++i) {
            data[i] = 0x47;
        }
        *transferred = bytes;
        return io[read_index++];
    }
    bool cancelled() const override { return cancel; }

    bool start_ok = true;
    bool stop_ok = true;
    bool cancel = false;
    std::vector<asicen::CaptureIo> io;
    std::vector<int> transfers;
    std::size_t read_index = 0;
};

class FakeOutput final : public asicen::CaptureOutput {
public:
    bool write(const unsigned char*, std::size_t size) override {
        if (fail) {
            return false;
        }
        written += size;
        return true;
    }

    bool fail = false;
    std::size_t written = 0;
};

asicen::CaptureRequest request(std::uint64_t limit, Clock::time_point deadline) {
    asicen::CaptureRequest req{};
    req.local = 1;
    req.endpoint = 0x82;
    req.byte_limit = limit;
    req.deadline = deadline;
    req.chunk_size = 4096;
    return req;
}

}  // namespace

int main() {
    const auto soon = [] { return Clock::now() + std::chrono::milliseconds(5); };
    const auto later = [] { return Clock::now() + std::chrono::seconds(1); };

    // Invalid arguments.
    {
        FakeBackend backend;
        FakeOutput output;
        auto req = request(0, later());
        req.endpoint = 0;
        check(asicen::run_raw_capture(&backend, &output, req, nullptr) ==
                  asicen::CaptureOutcome::InvalidArgument,
              "invalid endpoint");
        check(asicen::run_raw_capture(nullptr, &output, request(0, later()), nullptr) ==
                  asicen::CaptureOutcome::InvalidArgument,
              "null backend");
    }

    // Start failure.
    {
        FakeBackend backend;
        backend.start_ok = false;
        FakeOutput output;
        check(asicen::run_raw_capture(&backend, &output, request(0, later()), nullptr) ==
                  asicen::CaptureOutcome::StartFailed,
              "start failure");
    }

    // Zero bytes at deadline.
    {
        FakeBackend backend;
        FakeOutput output;
        asicen::CaptureStats stats{};
        check(asicen::run_raw_capture(&backend, &output, request(0, Clock::now()),
                                      &stats) == asicen::CaptureOutcome::ZeroBytes,
              "zero bytes");
        check(stats.bytes == 0, "zero stats");
    }

    // Limit reached -> Completed.
    {
        FakeBackend backend;
        backend.io = {asicen::CaptureIo::Ok, asicen::CaptureIo::Ok};
        backend.transfers = {4, 4};
        FakeOutput output;
        asicen::CaptureStats stats{};
        check(asicen::run_raw_capture(&backend, &output, request(8, later()), &stats) ==
                  asicen::CaptureOutcome::Completed,
              "limit completed");
        check(stats.bytes == 8 && stats.limit_reached, "limit stats");
        check(output.written == 8, "limit written");
    }

    // Partial timeout carrying bytes that reach the limit -> Completed.
    {
        FakeBackend backend;
        backend.io = {asicen::CaptureIo::Timeout};
        backend.transfers = {8};
        FakeOutput output;
        check(asicen::run_raw_capture(&backend, &output, request(8, later()), nullptr) ==
                  asicen::CaptureOutcome::Completed,
              "partial timeout limit");
        check(output.written == 8, "partial timeout written");
    }

    // Exact-limit USB error must not be hidden by the limit break.
    {
        FakeBackend backend;
        backend.io = {asicen::CaptureIo::Error};
        backend.transfers = {8};
        FakeOutput output;
        check(asicen::run_raw_capture(&backend, &output, request(8, later()), nullptr) ==
                  asicen::CaptureOutcome::UsbFailed,
              "exact-limit usb error");
        check(output.written == 8, "error bytes still written");
    }

    // Bytes captured but packet-count not reached before deadline.
    {
        FakeBackend backend;
        backend.io = {asicen::CaptureIo::Ok};
        backend.transfers = {4};
        FakeOutput output;
        asicen::CaptureStats stats{};
        check(asicen::run_raw_capture(&backend, &output, request(16, soon()), &stats) ==
                  asicen::CaptureOutcome::LimitNotReached,
              "limit not reached");
        check(stats.bytes == 4 && !stats.limit_reached, "limit-not-reached stats");
    }

    // Stop failure is authoritative.
    {
        FakeBackend backend;
        backend.stop_ok = false;
        backend.io = {asicen::CaptureIo::Ok};
        backend.transfers = {4};
        FakeOutput output;
        check(asicen::run_raw_capture(&backend, &output, request(4, later()), nullptr) ==
                  asicen::CaptureOutcome::StopFailed,
              "stop failure");
    }

    // Output failure (slow/disconnected consumer).
    {
        FakeBackend backend;
        backend.io = {asicen::CaptureIo::Ok};
        backend.transfers = {4};
        FakeOutput output;
        output.fail = true;
        check(asicen::run_raw_capture(&backend, &output, request(0, later()), nullptr) ==
                  asicen::CaptureOutcome::OutputFailed,
              "output failure");
    }

    // Cancellation.
    {
        FakeBackend backend;
        backend.cancel = true;
        FakeOutput output;
        check(asicen::run_raw_capture(&backend, &output, request(0, later()), nullptr) ==
                  asicen::CaptureOutcome::Cancelled,
              "cancelled");
    }

    return failures == 0 ? 0 : 1;
}
