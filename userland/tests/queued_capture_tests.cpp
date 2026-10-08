#include "asicen/queued_capture.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class Control final : public asicen::CaptureBackend {
public:
    explicit Control(std::vector<std::string>* trace) : trace_(trace) {}
    bool start_result = true;
    bool stop_result = true;
    bool cancel = false;
    std::uint8_t cf40 = 0xa4;
    int fail_cf_write_at = 0;
    int cf_write_count = 0;

    bool dsc_start(std::uint8_t) override {
        trace_->push_back("dsc-start");
        return start_result;
    }
    bool dsc_stop(std::uint8_t) override {
        trace_->push_back("dsc-stop");
        return stop_result;
    }
    asicen::CaptureIo bulk_read(std::uint8_t, unsigned char*, int, int*,
                                unsigned) override {
        return asicen::CaptureIo::Error;
    }
    bool read_cf40(std::uint8_t local, std::uint8_t* value) override {
        check(local == 1 && value != nullptr, "CF40 snapshot uses local 1");
        trace_->push_back("cf-read");
        *value = cf40;
        return true;
    }
    bool write_cf40(std::uint8_t local, std::uint8_t value) override {
        check(local == 1, "CF40 write uses local 1");
        trace_->push_back("cf-write:" + std::to_string(value));
        ++cf_write_count;
        // Model a possibly-partial control transfer: update state before
        // returning failure so restoration is independently verified.
        cf40 = value;
        return cf_write_count != fail_cf_write_at;
    }
    bool cancelled() const override { return cancel; }

private:
    std::vector<std::string>* trace_;
};

class Queue final : public asicen::QueuedCaptureIo {
public:
    explicit Queue(std::vector<std::string>* trace) : trace_(trace) {}
    std::array<unsigned char, 8> bytes{{'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'}};
    std::vector<asicen::QueueCompletion> events;
    std::size_t fail_submit = static_cast<std::size_t>(-1);
    bool fail_prepare = false;
    bool fail_resubmit = false;
    std::size_t cursor = 0;
    std::size_t in_flight = 0;
    std::size_t max_in_flight = 0;
    std::size_t depth = 0;
    bool released_after_drain = false;

    bool prepare(std::uint8_t endpoint, std::size_t wanted_depth,
                 std::size_t chunk_size) override {
        trace_->push_back("prepare");
        check(endpoint == 0x82, "endpoint passed to queue");
        check(chunk_size == 4096, "chunk size passed to queue");
        depth = wanted_depth;
        return !fail_prepare;
    }
    bool submit(std::size_t slot) override {
        trace_->push_back("submit" + std::to_string(slot));
        if (slot == fail_submit) return false;
        ++in_flight;
        if (in_flight > max_in_flight) max_in_flight = in_flight;
        return true;
    }
    asicen::QueueWait wait(unsigned, asicen::QueueCompletion* completion) override {
        if (cursor >= events.size()) return asicen::QueueWait::Timeout;
        *completion = events[cursor++];
        if (completion->io != asicen::CaptureIo::Error || completion->size != 0)
            --in_flight;
        return asicen::QueueWait::Completion;
    }
    bool resubmit(std::size_t slot) override {
        trace_->push_back("resubmit" + std::to_string(slot));
        if (fail_resubmit) return false;
        ++in_flight;
        if (in_flight > max_in_flight) max_in_flight = in_flight;
        return true;
    }
    void cancel_and_drain() override {
        trace_->push_back("cancel-drain");
        in_flight = 0;
        released_after_drain = true;
    }
    void release() override {
        check(released_after_drain || in_flight == 0,
              "transfer state released after drain or before any submit");
        trace_->push_back("release");
    }

private:
    std::vector<std::string>* trace_;
};

class Output final : public asicen::CaptureOutput {
public:
    bool accept = true;
    std::vector<unsigned char> bytes;
    bool write(const unsigned char* data, std::size_t size) override {
        if (!accept) return false;
        bytes.insert(bytes.end(), data, data + size);
        return true;
    }
};

asicen::CaptureRequest request(std::uint64_t byte_limit = 0) {
    return {1, 0x82, byte_limit, Clock::now() + std::chrono::seconds(1), 4096};
}

void successful_order_and_limit() {
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 4});
    asicen::CaptureStats stats{};
    const auto result = asicen::run_queued_capture(
        &control, &queue, &output, request(4), 4, &stats);
    check(result == asicen::CaptureOutcome::Completed, "queue capture completes at limit");
    check(output.bytes == std::vector<unsigned char>({'A', 'B', 'C', 'D'}),
          "queue writes completed bytes");
    check(stats.bytes == 4 && stats.limit_reached, "queue byte accounting");
    check(queue.max_in_flight == 4, "four transfers submitted concurrently");
    const std::vector<std::string> expected{
        "prepare", "submit0", "submit1", "submit2", "submit3", "dsc-start",
        "dsc-stop", "cancel-drain", "release"};
    check(trace == expected, "submit queue before DSC; stop, drain, then free");
}

void partial_timeout_reuses_slot_without_exceeding_depth() {
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    queue.events.push_back({0, asicen::CaptureIo::Timeout, queue.bytes.data(), 2});
    queue.events.push_back({1, asicen::CaptureIo::Ok, queue.bytes.data() + 2, 2});
    asicen::CaptureStats stats{};
    const auto result = asicen::run_queued_capture(
        &control, &queue, &output, request(4), 4, &stats);
    check(result == asicen::CaptureOutcome::Completed,
          "partial timed-out transfer can make progress and complete");
    check(output.bytes == std::vector<unsigned char>({'A', 'B', 'C', 'D'}),
          "partial timeout bytes are preserved in order");
    check(queue.max_in_flight == 4, "resubmission keeps queue depth bounded");
    check(trace[6] == "resubmit0", "completed slot resubmitted while capture continues");
}

void submit_failure_drains_partial_queue() {
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    queue.fail_submit = 2;
    const auto result = asicen::run_queued_capture(
        &control, &queue, &output, request(), 4, nullptr);
    check(result == asicen::CaptureOutcome::UsbFailed, "partial submit failure classified");
    check(trace == std::vector<std::string>({"prepare", "submit0", "submit1", "submit2",
                                             "cancel-drain", "release"}),
          "partial queue drained before release without DSC start");
}

void dsc_start_failure_stops_then_drains() {
    std::vector<std::string> trace;
    Control control(&trace);
    control.start_result = false;
    Queue queue(&trace);
    Output output;
    const auto result = asicen::run_queued_capture(
        &control, &queue, &output, request(), 4, nullptr);
    check(result == asicen::CaptureOutcome::UsbFailed, "DSC start failure classified");
    check(trace[5] == "dsc-start" && trace[6] == "dsc-stop" &&
              trace[7] == "cancel-drain" && trace[8] == "release",
          "failed DSC start attempts stop, then drains before free");
}

void exact_limit_error_is_not_success() {
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    queue.events.push_back({0, asicen::CaptureIo::Error, queue.bytes.data(), 4});
    asicen::CaptureStats stats{};
    const auto result = asicen::run_queued_capture(
        &control, &queue, &output, request(4), 4, &stats);
    check(result == asicen::CaptureOutcome::UsbFailed,
          "USB error carrying exact-limit bytes remains an error");
    check(stats.bytes == 4 && !stats.limit_reached,
          "error bytes accounted without masking exact-limit error");
    check(trace[trace.size() - 2] == "cancel-drain" && trace.back() == "release",
          "error path drains before release");
}

void output_failure_and_cancel_drain() {
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    output.accept = false;
    queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 2});
    const auto result = asicen::run_queued_capture(
        &control, &queue, &output, request(), 4, nullptr);
    check(result == asicen::CaptureOutcome::OutputFailed, "output error classified");
    check(trace[trace.size() - 3] == "dsc-stop" &&
              trace[trace.size() - 2] == "cancel-drain" && trace.back() == "release",
          "output error stop and drain order");
}

void cancelled_and_deadline_paths_drain() {
    {
        std::vector<std::string> trace;
        Control control(&trace);
        control.cancel = true;
        Queue queue(&trace);
        Output output;
        const auto result = asicen::run_queued_capture(
            &control, &queue, &output, request(), 4, nullptr);
        check(result == asicen::CaptureOutcome::Cancelled, "signal cancellation classified");
        check(trace[trace.size() - 3] == "dsc-stop" &&
                  trace[trace.size() - 2] == "cancel-drain" && trace.back() == "release",
              "cancellation stops and drains before release");
    }
    {
        std::vector<std::string> trace;
        Control control(&trace);
        Queue queue(&trace);
        Output output;
        auto expired = request();
        expired.deadline = Clock::now() - std::chrono::milliseconds(1);
        const auto result = asicen::run_queued_capture(
            &control, &queue, &output, expired, 4, nullptr);
        check(result == asicen::CaptureOutcome::ZeroBytes, "deadline with no bytes classified");
        check(trace[trace.size() - 3] == "dsc-stop" &&
                  trace[trace.size() - 2] == "cancel-drain" && trace.back() == "release",
              "deadline stops and drains before release");
    }
}

void resubmit_and_stop_failures_remain_errors() {
    {
        std::vector<std::string> trace;
        Control control(&trace);
        Queue queue(&trace);
        Output output;
        queue.fail_resubmit = true;
        queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 2});
        asicen::CaptureStats stats{};
        const auto result = asicen::run_queued_capture(
            &control, &queue, &output, request(), 4, &stats);
        check(result == asicen::CaptureOutcome::UsbFailed, "resubmit failure classified");
        check(stats.bytes == 2, "partial data before resubmit failure retained");
        check(trace[trace.size() - 2] == "cancel-drain" && trace.back() == "release",
              "resubmit failure drains before release");
    }
    {
        std::vector<std::string> trace;
        Control control(&trace);
        control.stop_result = false;
        Queue queue(&trace);
        Output output;
        queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 4});
        const auto result = asicen::run_queued_capture(
            &control, &queue, &output, request(4), 4, nullptr);
        check(result == asicen::CaptureOutcome::StopFailed, "DSC stop failure classified");
        check(trace[trace.size() - 2] == "cancel-drain" && trace.back() == "release",
              "DSC stop failure still drains before release");
    }
}

void invalid_depth_does_not_allocate() {
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    const auto result = asicen::run_queued_capture(
        &control, &queue, &output, request(), 5, nullptr);
    check(result == asicen::CaptureOutcome::InvalidArgument,
          "queue depth over kernel maximum rejected");
    check(trace.empty(), "invalid queue depth rejected before allocation");
}

void prepare_failure_releases_without_starting() {
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    queue.fail_prepare = true;
    Output output;
    const auto result = asicen::run_queued_capture(
        &control, &queue, &output, request(), 4, nullptr);
    check(result == asicen::CaptureOutcome::UsbFailed, "allocation failure classified");
    check(trace == std::vector<std::string>({"prepare", "release"}),
          "unsubmitted allocation releases without DSC or cancellation");
}

void filter_start_orders_rmw_and_restores_snapshot() {
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 4});
    const auto result = asicen::run_queued_capture(
        &control, &queue, &output, request(4), 4, nullptr, true);
    check(result == asicen::CaptureOutcome::Completed,
          "filter-start diagnostic completes with data");
    check(control.cf40 == 0xa4, "filter-start restores original CF40 byte");
    const std::vector<std::string> expected{
        "cf-read", "cf-read", "cf-write:167", "prepare", "submit0", "submit1",
        "submit2", "submit3", "dsc-start", "cf-read", "cf-write:175",
        "dsc-stop", "cancel-drain", "cf-write:164", "release"};
    check(trace == expected,
          "CF40 selector bits precede queued DSC and bit 3 follows successful start");
}

void filter_start_restores_after_dsc_start_failure() {
    std::vector<std::string> trace;
    Control control(&trace);
    control.start_result = false;
    Queue queue(&trace);
    Output output;
    const auto result = asicen::run_queued_capture(
        &control, &queue, &output, request(), 4, nullptr, true);
    check(result == asicen::CaptureOutcome::UsbFailed,
          "filter-start DSC failure remains an error");
    check(control.cf40 == 0xa4, "DSC start failure restores original CF40");
    check(trace[8] == "dsc-start" && trace[9] == "dsc-stop" &&
              trace[10] == "cancel-drain" && trace[11] == "cf-write:164" &&
              trace[12] == "release",
          "failed DSC start stops, drains callbacks, restores CF40, then releases");
}

void filter_start_write_failures_restore_snapshot() {
    {
        std::vector<std::string> trace;
        Control control(&trace);
        control.fail_cf_write_at = 1;
        Queue queue(&trace);
        Output output;
        const auto result = asicen::run_queued_capture(
            &control, &queue, &output, request(), 4, nullptr, true);
        check(result == asicen::CaptureOutcome::UsbFailed,
              "pre-start CF40 write failure classified");
        check(control.cf40 == 0xa4, "pre-start CF40 failure restored snapshot");
        check(trace == std::vector<std::string>(
                           {"cf-read", "cf-read", "cf-write:167", "cf-write:164"}),
              "failed pre-start CF40 write restores before allocating transfers");
    }
    {
        std::vector<std::string> trace;
        Control control(&trace);
        control.fail_cf_write_at = 2;
        Queue queue(&trace);
        Output output;
        const auto result = asicen::run_queued_capture(
            &control, &queue, &output, request(), 4, nullptr, true);
        check(result == asicen::CaptureOutcome::UsbFailed,
              "post-start CF40 write failure classified");
        check(control.cf40 == 0xa4, "post-start CF40 failure restored snapshot");
        check(trace[8] == "dsc-start" && trace[9] == "cf-read" &&
                  trace[10] == "cf-write:175" && trace[11] == "dsc-stop" &&
                  trace[12] == "cancel-drain" && trace[13] == "cf-write:164" &&
                  trace[14] == "release",
              "post-start CF40 failure stops, drains, restores, then releases");
    }
}

void cf40_response_uses_transfer_length_not_status_byte() {
    const unsigned char response[2] = {0x00, 0x5a};
    std::uint8_t value = 0;
    check(asicen::parse_cf40_read_response(2, response, &value) && value == 0x5a,
          "CF40 read consumes payload after full transfer regardless of status byte");
    check(!asicen::parse_cf40_read_response(1, response, &value),
          "short CF40 read is rejected");
    check(asicen::cf40_write_response_complete(2),
          "CF40 write accepts complete WDM transfer regardless of first byte");
    check(!asicen::cf40_write_response_complete(1), "short CF40 write is rejected");
}

void filter_restore_failure_is_reported() {
    std::vector<std::string> trace;
    Control control(&trace);
    control.fail_cf_write_at = 3;
    Queue queue(&trace);
    Output output;
    queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 4});
    asicen::CaptureStats stats{};
    const auto result = asicen::run_queued_capture(
        &control, &queue, &output, request(4), 4, &stats, true);
    check(result == asicen::CaptureOutcome::UsbFailed,
          "restore failure prevents a successful capture outcome");
    check(stats.cf40_restore_failed,
          "capture stats preserve explicit CF40 restoration failure");
    check(trace[11] == "dsc-stop" && trace[12] == "cancel-drain" &&
              trace[13] == "cf-write:164" && trace[14] == "release",
          "restoration failure still follows DSC stop and callback drain");
}

}  // namespace

int main() {
    successful_order_and_limit();
    partial_timeout_reuses_slot_without_exceeding_depth();
    submit_failure_drains_partial_queue();
    dsc_start_failure_stops_then_drains();
    exact_limit_error_is_not_success();
    output_failure_and_cancel_drain();
    cancelled_and_deadline_paths_drain();
    resubmit_and_stop_failures_remain_errors();
    invalid_depth_does_not_allocate();
    prepare_failure_releases_without_starting();
    filter_start_orders_rmw_and_restores_snapshot();
    filter_start_restores_after_dsc_start_failure();
    filter_start_write_failures_restore_snapshot();
    cf40_response_uses_transfer_length_not_status_byte();
    filter_restore_failure_is_reported();
    std::cout << "queued capture lifecycle tests passed\n";
    return 0;
}
