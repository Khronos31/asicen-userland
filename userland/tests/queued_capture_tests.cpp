#include "asicen/queued_capture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

bool check(bool value, const char* message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!check(__VA_ARGS__)) {                                                                 \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

class Control final : public asicen::CaptureBackend {
public:
    explicit Control(std::vector<std::string>* trace) : trace_(trace)
    {
        block.fill(0);
        block[0] = 0x11;
        block[0x40] = cf40;
        block[0x44] = 0x42;
    }
    bool start_result = true;
    bool stop_result = true;
    bool cancel = false;
    std::uint8_t cf40 = 0xa4;
    int fail_cf_write_at = 0;
    int cf_write_count = 0;
    int cf_read_count = 0;
    int fail_cf_read_at = 0;
    std::array<std::uint8_t, 0x45> block{};
    std::array<std::uint8_t, 0x45> restored_block{};
    bool fail_repeat = false;
    bool fail_full_block_write = false;
    std::vector<bool> lock_results;
    std::size_t lock_cursor = 0;
    int full_block_writes = 0;
    std::uint8_t pulse_cf40 = 0;
    bool fail_link_snapshot = false;
    bool fail_link_apply = false;
    bool fail_link_cleanup = false;

    bool dsc_start(std::uint8_t) override
    {
        trace_->push_back("dsc-start");
        return start_result;
    }
    bool dsc_stop(std::uint8_t) override
    {
        trace_->push_back("dsc-stop");
        return stop_result;
    }
    asicen::CaptureIo bulk_read(std::uint8_t, unsigned char*, int, int*, unsigned) override
    {
        return asicen::CaptureIo::Error;
    }
    bool read_cf40(std::uint8_t local, std::uint8_t* value) override
    {
        CHECK(local == 1 && value != nullptr, "CF40 snapshot uses local 1");
        trace_->push_back("cf-read");
        ++cf_read_count;
        if (cf_read_count == fail_cf_read_at) {
            return false;
        }
        cf40 = block[0x40];
        *value = cf40;
        return true;
    }
    bool write_cf40(std::uint8_t local, std::uint8_t value) override
    {
        CHECK(local == 1, "CF40 write uses local 1");
        trace_->push_back("cf-write:" + std::to_string(value));
        ++cf_write_count;
        // Model a possibly-partial control transfer: update state before
        // returning failure so restoration is independently verified.
        cf40 = value;
        block[0x40] = value;
        return cf_write_count != fail_cf_write_at;
    }
    bool read_cf_block(std::uint8_t local, std::uint8_t* data, std::size_t size) override
    {
        CHECK(local == 1 && data != nullptr && size == block.size(),
              "repeat block snapshot uses local 1 and full CF extent");
        trace_->push_back("block-read");
        std::copy(block.begin(), block.end(), data);
        return true;
    }
    bool write_cf_block(std::uint8_t local, const std::uint8_t* data, std::size_t size) override
    {
        CHECK(local == 1 && data != nullptr && size == block.size(),
              "repeat restoration writes complete CF extent");
        trace_->push_back("block-restore");
        ++full_block_writes;
        if (fail_full_block_write) {
            return false;
        }
        std::copy(data, data + size, block.begin());
        restored_block = block;
        cf40 = block[0x40];
        return true;
    }
    bool terrestrial_locked(std::uint8_t local, bool* locked,
                            std::chrono::steady_clock::time_point) override
    {
        CHECK(local == 1 && locked != nullptr, "repeat lock check uses lane 1");
        trace_->push_back("lock");
        const bool result = lock_cursor < lock_results.size() ? lock_results[lock_cursor++] : true;
        *locked = result;
        return true;
    }
    bool filter_repeat_pulse(std::uint8_t local, std::uint8_t reset_state,
                             std::chrono::steady_clock::time_point) override
    {
        CHECK(local == 1 && reset_state == 1, "repeat P uses reset state 1 on lane 1");
        trace_->push_back("P");
        if (fail_repeat) {
            return false;
        }
        block[0x40] = static_cast<std::uint8_t>(block[0x40] | 0x04U);
        block[0x40] = static_cast<std::uint8_t>(block[0x40] | 0x03U);
        cf40 = block[0x40];
        pulse_cf40 = cf40;
        return true;
    }
    bool snapshot_link_diagnostic() override
    {
        trace_->push_back("link-snapshot");
        return !fail_link_snapshot;
    }
    bool apply_link_seed() override
    {
        trace_->push_back("link-apply");
        return !fail_link_apply;
    }
    bool clear_link_seed_and_verify_controller() override
    {
        trace_->push_back("link-cleanup-zero-and-05-readback");
        return !fail_link_cleanup;
    }
    bool cancelled() const override { return cancel; }

private:
    std::vector<std::string>* trace_;
};

class PrimaryLaneControl final : public asicen::CaptureBackend {
public:
    bool dsc_start(std::uint8_t local) override
    {
        start_local = local;
        return local == 0U;
    }
    bool dsc_stop(std::uint8_t local) override
    {
        stop_local = local;
        return local == 0U;
    }
    asicen::CaptureIo bulk_read(std::uint8_t, unsigned char*, int, int*, unsigned) override
    {
        return asicen::CaptureIo::Error;
    }
    std::uint8_t start_local = 0xffU;
    std::uint8_t stop_local = 0xffU;
};

class Queue final : public asicen::QueuedCaptureIo {
public:
    explicit Queue(std::vector<std::string>* trace, std::uint8_t expected_endpoint = 0x82U)
        : trace_(trace), expected_endpoint_(expected_endpoint)
    {
    }
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
    asicen::QueueObservation* observation = nullptr;
    std::array<std::uint64_t, 4> generations{};
    std::array<bool, 4> slot_pending{};

    bool prepare(std::uint8_t endpoint, std::size_t wanted_depth, std::size_t chunk_size) override
    {
        trace_->push_back("prepare");
        CHECK(endpoint == expected_endpoint_, "endpoint passed to queue");
        CHECK(chunk_size == 4096, "chunk size passed to queue");
        depth = wanted_depth;
        return !fail_prepare;
    }
    bool submit(std::size_t slot) override
    {
        trace_->push_back("submit" + std::to_string(slot));
        if (slot == fail_submit) {
            return false;
        }
        ++generations[slot];
        slot_pending[slot] = true;
        ++in_flight;
        if (in_flight > max_in_flight) {
            max_in_flight = in_flight;
        }
        return true;
    }
    asicen::QueueWait wait(unsigned, asicen::QueueCompletion* completion) override
    {
        if (cursor >= events.size()) {
            return asicen::QueueWait::Timeout;
        }
        *completion = events[cursor++];
        slot_pending[completion->slot] = false;
        if (observation != nullptr) {
            observation->record_callback(completion->slot, completion->generation,
                                         completion->raw_status, completion->requested_length,
                                         completion->actual_length);
        }
        if (completion->io != asicen::CaptureIo::Error || completion->size != 0) {
            --in_flight;
        }
        return asicen::QueueWait::Completion;
    }
    bool resubmit(std::size_t slot) override
    {
        trace_->push_back("resubmit" + std::to_string(slot));
        if (fail_resubmit) {
            return false;
        }
        ++generations[slot];
        slot_pending[slot] = true;
        ++in_flight;
        if (in_flight > max_in_flight) {
            max_in_flight = in_flight;
        }
        return true;
    }
    void set_observation(asicen::QueueObservation* value) override { observation = value; }
    void snapshot_before_stop() override
    {
        if (observation != nullptr) {
            observation->record_before_stop(in_flight, events.size() - cursor);
        }
    }
    void set_phase(asicen::QueuePhase phase) override
    {
        if (observation != nullptr) {
            observation->set_phase(phase);
        }
    }
    void cancel_and_drain() override
    {
        trace_->push_back("cancel-drain");
        if (observation != nullptr) {
            for (std::size_t slot = 0; slot < depth; ++slot) {
                if (slot_pending[slot]) {
                    observation->record_cancel(slot, generations[slot], -5);
                }
            }
        }
        in_flight = 0;
        released_after_drain = true;
    }
    void release() override
    {
        if (!check(released_after_drain || in_flight == 0,
                   "transfer state released after drain or before any submit")) {
            return;
        }
        trace_->push_back("release");
    }

private:
    std::vector<std::string>* trace_;
    std::uint8_t expected_endpoint_;
};

class Output final : public asicen::CaptureOutput {
public:
    bool accept = true;
    std::vector<unsigned char> bytes;
    bool write(const unsigned char* data, std::size_t size) override
    {
        if (!accept) {
            return false;
        }
        bytes.insert(bytes.end(), data, data + size);
        return true;
    }
};

asicen::CaptureRequest request(std::uint64_t byte_limit = 0)
{
    return {1, 0x82, byte_limit, Clock::now() + std::chrono::seconds(1), 4096};
}

bool successful_order_and_limit()
{
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 4});
    asicen::CaptureStats stats{};
    const auto result =
        asicen::run_queued_capture(&control, &queue, &output, request(4), 4, &stats);
    CHECK(result == asicen::CaptureOutcome::Completed, "queue capture completes at limit");
    CHECK(output.bytes == std::vector<unsigned char>({'A', 'B', 'C', 'D'}),
          "queue writes completed bytes");
    CHECK(stats.bytes == 4 && stats.limit_reached, "queue byte accounting");
    CHECK(queue.max_in_flight == 4, "four transfers submitted concurrently");
    const std::vector<std::string> expected{"prepare",  "submit0",      "submit1",
                                            "submit2",  "submit3",      "dsc-start",
                                            "dsc-stop", "cancel-drain", "release"};
    CHECK(trace == expected, "submit queue before DSC; stop, drain, then free");
    return true;
}

bool primary_satellite_capture_uses_lane0_endpoint_and_dsc()
{
    std::vector<std::string> trace;
    PrimaryLaneControl control;
    Queue queue(&trace, 0x81U);
    Output output;
    queue.events.push_back({0U, asicen::CaptureIo::Ok, queue.bytes.data(), 4U});
    asicen::CaptureStats stats{};
    asicen::CaptureRequest primary{0U, 0x81U, 4U, Clock::now() + std::chrono::seconds(1), 4096U};
    CHECK(asicen::run_queued_capture(&control, &queue, &output, primary, 4U, &stats) ==
              asicen::CaptureOutcome::Completed,
          "primary satellite lane capture completes");
    CHECK(control.start_local == 0U && control.stop_local == 0U,
          "both DSC start and cleanup stop address primary local0");
    CHECK(queue.max_in_flight == 4U && stats.bytes == 4U,
          "primary lane retains the four-slot capture lifecycle");
    return true;
}

bool partial_timeout_reuses_slot_without_exceeding_depth()
{
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    queue.events.push_back({0, asicen::CaptureIo::Timeout, queue.bytes.data(), 2});
    queue.events.push_back({1, asicen::CaptureIo::Ok, queue.bytes.data() + 2, 4});
    asicen::CaptureStats stats{};
    const auto result =
        asicen::run_queued_capture(&control, &queue, &output, request(4), 4, &stats);
    CHECK(result == asicen::CaptureOutcome::Completed,
          "successful transfer after a timeout can complete capture");
    CHECK(output.bytes == std::vector<unsigned char>({'C', 'D', 'E', 'F'}),
          "partial timeout bytes are discarded before the successful completion");
    CHECK(queue.max_in_flight == 4, "resubmission keeps queue depth bounded");
    CHECK(trace[6] == "resubmit0", "completed slot resubmitted while capture continues");
    return true;
}

bool submit_failure_drains_partial_queue()
{
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    queue.fail_submit = 2;
    const auto result =
        asicen::run_queued_capture(&control, &queue, &output, request(), 4, nullptr);
    CHECK(result == asicen::CaptureOutcome::UsbFailed, "partial submit failure classified");
    CHECK(trace == std::vector<std::string>(
                       {"prepare", "submit0", "submit1", "submit2", "cancel-drain", "release"}),
          "partial queue drained before release without DSC start");
    return true;
}

bool dsc_start_failure_stops_then_drains()
{
    std::vector<std::string> trace;
    Control control(&trace);
    control.start_result = false;
    Queue queue(&trace);
    Output output;
    const auto result =
        asicen::run_queued_capture(&control, &queue, &output, request(), 4, nullptr);
    CHECK(result == asicen::CaptureOutcome::UsbFailed, "DSC start failure classified");
    CHECK(trace[5] == "dsc-start" && trace[6] == "dsc-stop" && trace[7] == "cancel-drain" &&
              trace[8] == "release",
          "failed DSC start attempts stop, then drains before free");
    return true;
}

bool exact_limit_error_is_not_success()
{
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    queue.events.push_back({0, asicen::CaptureIo::Error, queue.bytes.data(), 4});
    asicen::CaptureStats stats{};
    const auto result =
        asicen::run_queued_capture(&control, &queue, &output, request(4), 4, &stats);
    CHECK(result == asicen::CaptureOutcome::UsbFailed,
          "USB error carrying exact-limit bytes remains an error");
    CHECK(stats.bytes == 0 && !stats.limit_reached && output.bytes.empty(),
          "partial error bytes are rejected before output");
    CHECK(trace[trace.size() - 2] == "cancel-drain" && trace.back() == "release",
          "error path drains before release");
    return true;
}

bool output_failure_and_cancel_drain()
{
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    output.accept = false;
    queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 2});
    const auto result =
        asicen::run_queued_capture(&control, &queue, &output, request(), 4, nullptr);
    CHECK(result == asicen::CaptureOutcome::OutputFailed, "output error classified");
    CHECK(trace[trace.size() - 3] == "dsc-stop" && trace[trace.size() - 2] == "cancel-drain" &&
              trace.back() == "release",
          "output error stop and drain order");
    return true;
}

bool cancelled_and_deadline_paths_drain()
{
    {
        std::vector<std::string> trace;
        Control control(&trace);
        control.cancel = true;
        Queue queue(&trace);
        Output output;
        const auto result =
            asicen::run_queued_capture(&control, &queue, &output, request(), 4, nullptr);
        CHECK(result == asicen::CaptureOutcome::Cancelled, "signal cancellation classified");
        CHECK(trace[trace.size() - 3] == "dsc-stop" && trace[trace.size() - 2] == "cancel-drain" &&
                  trace.back() == "release",
              "cancellation stops and drains before release");
    }
    {
        std::vector<std::string> trace;
        Control control(&trace);
        Queue queue(&trace);
        Output output;
        auto expired = request();
        expired.deadline = Clock::now() - std::chrono::milliseconds(1);
        const auto result =
            asicen::run_queued_capture(&control, &queue, &output, expired, 4, nullptr);
        CHECK(result == asicen::CaptureOutcome::ZeroBytes, "deadline with no bytes classified");
        CHECK(trace[trace.size() - 3] == "dsc-stop" && trace[trace.size() - 2] == "cancel-drain" &&
                  trace.back() == "release",
              "deadline stops and drains before release");
    }
    return true;
}

bool resubmit_and_stop_failures_remain_errors()
{
    {
        std::vector<std::string> trace;
        Control control(&trace);
        Queue queue(&trace);
        Output output;
        queue.fail_resubmit = true;
        queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 2});
        asicen::CaptureStats stats{};
        const auto result =
            asicen::run_queued_capture(&control, &queue, &output, request(), 4, &stats);
        CHECK(result == asicen::CaptureOutcome::UsbFailed, "resubmit failure classified");
        CHECK(stats.bytes == 2, "partial data before resubmit failure retained");
        CHECK(trace[trace.size() - 2] == "cancel-drain" && trace.back() == "release",
              "resubmit failure drains before release");
    }
    {
        std::vector<std::string> trace;
        Control control(&trace);
        control.stop_result = false;
        Queue queue(&trace);
        Output output;
        queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 4});
        const auto result =
            asicen::run_queued_capture(&control, &queue, &output, request(4), 4, nullptr);
        CHECK(result == asicen::CaptureOutcome::StopFailed, "DSC stop failure classified");
        CHECK(trace[trace.size() - 2] == "cancel-drain" && trace.back() == "release",
              "DSC stop failure still drains before release");
    }
    return true;
}

bool invalid_depth_does_not_allocate()
{
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    const auto result =
        asicen::run_queued_capture(&control, &queue, &output, request(), 5, nullptr);
    CHECK(result == asicen::CaptureOutcome::InvalidArgument,
          "queue depth over kernel maximum rejected");
    CHECK(trace.empty(), "invalid queue depth rejected before allocation");
    return true;
}

bool prepare_failure_releases_without_starting()
{
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    queue.fail_prepare = true;
    Output output;
    const auto result =
        asicen::run_queued_capture(&control, &queue, &output, request(), 4, nullptr);
    CHECK(result == asicen::CaptureOutcome::UsbFailed, "allocation failure classified");
    CHECK(trace == std::vector<std::string>({"prepare", "release"}),
          "unsubmitted allocation releases without DSC or cancellation");
    return true;
}

bool filter_start_orders_rmw_and_restores_snapshot()
{
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 4});
    const auto result =
        asicen::run_queued_capture(&control, &queue, &output, request(4), 4, nullptr, true);
    CHECK(result == asicen::CaptureOutcome::Completed,
          "filter-start diagnostic completes with data");
    CHECK(control.cf40 == 0xa4, "filter-start restores original CF40 byte");
    const std::vector<std::string> expected{
        "cf-read",      "cf-read",  "cf-write:167", "prepare",      "submit0",
        "submit1",      "submit2",  "submit3",      "dsc-start",    "cf-read",
        "cf-write:175", "dsc-stop", "cancel-drain", "cf-write:164", "release"};
    CHECK(trace == expected,
          "CF40 selector bits precede queued DSC and bit 3 follows successful start");
    return true;
}

bool filter_start_restores_after_dsc_start_failure()
{
    std::vector<std::string> trace;
    Control control(&trace);
    control.start_result = false;
    Queue queue(&trace);
    Output output;
    const auto result =
        asicen::run_queued_capture(&control, &queue, &output, request(), 4, nullptr, true);
    CHECK(result == asicen::CaptureOutcome::UsbFailed, "filter-start DSC failure remains an error");
    CHECK(control.cf40 == 0xa4, "DSC start failure restores original CF40");
    CHECK(trace[8] == "dsc-start" && trace[9] == "dsc-stop" && trace[10] == "cancel-drain" &&
              trace[11] == "cf-write:164" && trace[12] == "release",
          "failed DSC start stops, drains callbacks, restores CF40, then releases");
    return true;
}

bool filter_start_write_failures_restore_snapshot()
{
    {
        std::vector<std::string> trace;
        Control control(&trace);
        control.fail_cf_write_at = 1;
        Queue queue(&trace);
        Output output;
        const auto result =
            asicen::run_queued_capture(&control, &queue, &output, request(), 4, nullptr, true);
        CHECK(result == asicen::CaptureOutcome::UsbFailed,
              "pre-start CF40 write failure classified");
        CHECK(control.cf40 == 0xa4, "pre-start CF40 failure restored snapshot");
        CHECK(trace ==
                  std::vector<std::string>({"cf-read", "cf-read", "cf-write:167", "cf-write:164"}),
              "failed pre-start CF40 write restores before allocating transfers");
    }
    {
        std::vector<std::string> trace;
        Control control(&trace);
        control.fail_cf_write_at = 2;
        Queue queue(&trace);
        Output output;
        const auto result =
            asicen::run_queued_capture(&control, &queue, &output, request(), 4, nullptr, true);
        CHECK(result == asicen::CaptureOutcome::UsbFailed,
              "post-start CF40 write failure classified");
        CHECK(control.cf40 == 0xa4, "post-start CF40 failure restored snapshot");
        CHECK(trace[8] == "dsc-start" && trace[9] == "cf-read" && trace[10] == "cf-write:175" &&
                  trace[11] == "dsc-stop" && trace[12] == "cancel-drain" &&
                  trace[13] == "cf-write:164" && trace[14] == "release",
              "post-start CF40 failure stops, drains, restores, then releases");
    }
    return true;
}

bool cf40_response_uses_transfer_length_not_status_byte()
{
    const unsigned char response[2] = {0x00, 0x5a};
    std::uint8_t value = 0;
    CHECK(asicen::parse_cf40_read_response(2, response, &value) && value == 0x5a,
          "CF40 read consumes payload after full transfer regardless of status byte");
    CHECK(!asicen::parse_cf40_read_response(1, response, &value), "short CF40 read is rejected");
    CHECK(asicen::cf40_write_response_complete(2),
          "CF40 write accepts complete WDM transfer regardless of first byte");
    CHECK(!asicen::cf40_write_response_complete(1), "short CF40 write is rejected");
    return true;
}

bool filter_restore_failure_is_reported()
{
    std::vector<std::string> trace;
    Control control(&trace);
    control.fail_cf_write_at = 3;
    Queue queue(&trace);
    Output output;
    queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 4});
    asicen::CaptureStats stats{};
    const auto result =
        asicen::run_queued_capture(&control, &queue, &output, request(4), 4, &stats, true);
    CHECK(result == asicen::CaptureOutcome::UsbFailed,
          "restore failure prevents a successful capture outcome");
    CHECK(stats.cf40_restore_failed, "capture stats preserve explicit CF40 restoration failure");
    CHECK(trace[11] == "dsc-stop" && trace[12] == "cancel-drain" && trace[13] == "cf-write:164" &&
              trace[14] == "release",
          "restoration failure still follows DSC stop and callback drain");
    return true;
}

bool link_seed_is_applied_after_dsc_and_zeroed_after_drain()
{
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 4});
    asicen::CaptureStats stats{};
    const auto original = control.block;
    const auto result = asicen::run_queued_capture(&control, &queue, &output, request(4), 4, &stats,
                                                   true, &original[0x40], nullptr,
                                                   asicen::FilterRepeat::None, 1, nullptr, true);
    const auto position = [&](const char* value) {
        const auto it = std::find(trace.begin(), trace.end(), value);
        return static_cast<std::size_t>(std::distance(trace.begin(), it));
    };
    CHECK(result == asicen::CaptureOutcome::Completed,
          "link seed queue capture completes and runs acknowledged cleanup");
    CHECK(position("link-snapshot") < position("prepare") &&
              position("dsc-start") < position("link-apply") &&
              position("link-apply") < position("dsc-stop") &&
              position("cancel-drain") < position("link-cleanup-zero-and-05-readback") &&
              position("link-cleanup-zero-and-05-readback") < position("release"),
          "link snapshot, DSC/apply and stop/drain/zero-cleanup ordering is enforced");
    CHECK(!stats.link_seed_cleanup_failed && !stats.link_seed_apply_failed &&
              stats.link_seed_state_unverifiable,
          "cleanup passes while seed-register state remains explicitly unverifiable");
    return true;
}

bool link_seed_partial_apply_and_cleanup_failures_are_reported()
{
    for (const bool cleanup_failure : {false, true}) {
        std::vector<std::string> trace;
        Control control(&trace);
        control.fail_link_apply = true;
        control.fail_link_cleanup = cleanup_failure;
        Queue queue(&trace);
        Output output;
        asicen::CaptureStats stats{};
        const auto original = control.block;
        const auto result = asicen::run_queued_capture(
            &control, &queue, &output, request(), 4, &stats, true, &original[0x40], nullptr,
            asicen::FilterRepeat::None, 1, nullptr, true);
        CHECK(result == asicen::CaptureOutcome::UsbFailed && stats.link_seed_apply_failed,
              "link apply failure is an explicit capture failure");
        CHECK(stats.link_seed_cleanup_failed == cleanup_failure,
              "link cleanup result is separately reported");
        const auto drain = std::find(trace.begin(), trace.end(), "cancel-drain");
        const auto cleanup =
            std::find(trace.begin(), trace.end(), "link-cleanup-zero-and-05-readback");
        CHECK(drain != trace.end() && cleanup != trace.end() && drain < cleanup,
              "partial link writes are zeroed only after transfer drain");
    }
    return true;
}

bool link_seed_is_not_cleared_if_dsc_stop_fails()
{
    std::vector<std::string> trace;
    Control control(&trace);
    control.stop_result = false;
    Queue queue(&trace);
    Output output;
    queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 4});
    asicen::CaptureStats stats{};
    const auto original = control.block;
    const auto result = asicen::run_queued_capture(&control, &queue, &output, request(4), 4, &stats,
                                                   true, &original[0x40], nullptr,
                                                   asicen::FilterRepeat::None, 1, nullptr, true);
    CHECK(result == asicen::CaptureOutcome::StopFailed && stats.link_seed_cleanup_failed,
          "failed DSC stop prevents link cleanup and fails capture");
    CHECK(std::find(trace.begin(), trace.end(), "link-cleanup-zero-and-05-readback") == trace.end(),
          "seed writes are not zeroed while device-side stream may remain active");
    CHECK(std::find(trace.begin(), trace.end(), "cancel-drain") != trace.end(),
          "host transfers are still drained after DSC stop failure");
    return true;
}

bool link_snapshot_failure_restores_filter_snapshot()
{
    std::vector<std::string> trace;
    Control control(&trace);
    control.fail_link_snapshot = true;
    control.cf40 = 0x03;
    control.block[0x40] = 0x03;
    const std::uint8_t original_cf40 = 0xa4;
    Queue queue(&trace);
    Output output;
    asicen::CaptureStats stats{};
    const auto result = asicen::run_queued_capture(&control, &queue, &output, request(), 4, &stats,
                                                   true, &original_cf40, nullptr,
                                                   asicen::FilterRepeat::None, 1, nullptr, true);
    CHECK(result == asicen::CaptureOutcome::UsbFailed && control.cf40 == original_cf40 &&
              !stats.cf40_restore_failed,
          "link snapshot failure still restores CF40 reset by outer setup");
    CHECK(std::find(trace.begin(), trace.end(), "prepare") == trace.end(),
          "capture queue is not prepared after link snapshot failure");
    return true;
}

bool queue_observation_accounts_callbacks_once_and_bounds_events()
{
    asicen::QueueObservation observation;
    observation.record_callback(0, 1, 0, 4096, 12);
    observation.record_callback(0, 1, 0, 4096, 12);  // duplicate callback
    observation.set_phase(asicen::QueuePhase::StoppingDsc);
    observation.record_callback(1, 1, 3, 4096, 7);  // cancelled partial completion
    observation.set_phase(asicen::QueuePhase::CancelDrain);
    observation.record_callback(0, 2, 3, 4096, 5);  // next generation
    for (std::uint64_t generation = 2; generation < 260; ++generation) {
        observation.record_callback(2, generation, 0, 4096, 1);
    }
    observation.record_before_stop(2, 1);
    observation.record_cancel(1, 1, -5);  // libusb NOT_FOUND
    CHECK(observation.callback_count == 261,
          "unique callback generations counted once across reused slots");
    CHECK(observation.duplicate_or_stale_callbacks == 1,
          "duplicate generation is excluded from accounting");
    CHECK(observation.callback_actual_bytes == 282,
          "actual byte totals include partial and drained callback payloads");
    CHECK(observation.phase_counts[0] == 1 && observation.phase_counts[1] == 1 &&
              observation.phase_counts[2] == 259,
          "callbacks retain the phase observed at entry");
    CHECK(observation.event_count == asicen::QueueObservation::kMaxEvents &&
              observation.event_overflow == 5,
          "event storage is capped while all callback totals continue");
    CHECK(observation.pending_before_stop == 2 && observation.ready_before_stop == 1,
          "pre-stop pending and ready counts are retained");
    CHECK(observation.cancellation_count == 1 && observation.cancellations[0].return_code == -5,
          "cancel return codes including NOT_FOUND are retained");
    return true;
}

bool queue_observation_preserves_normal_capture_accounting()
{
    std::vector<std::string> trace;
    Control control(&trace);
    Queue queue(&trace);
    Output output;
    queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 4, 1, 0, 4096, 4});
    asicen::QueueObservation observation;
    asicen::CaptureStats stats{};
    const auto result = asicen::run_queued_capture(&control, &queue, &output, request(4), 4, &stats,
                                                   false, nullptr, &observation);
    CHECK(result == asicen::CaptureOutcome::Completed,
          "observation-enabled queue retains successful capture outcome");
    CHECK(stats.bytes == 4 && output.bytes.size() == 4,
          "diagnostics do not change bytes written to the capture output");
    CHECK(observation.callback_count == 1 && observation.normal_delivery_count == 1 &&
              observation.normal_delivery_actual_bytes == 4,
          "normal callback and normal delivery are accounted independently");
    CHECK(observation.pending_before_stop == 3 && observation.ready_before_stop == 0 &&
              observation.cancellation_count == 3,
          "capture snapshots queued work and records each cancel attempt");
    return true;
}

bool queue_observation_counts_handoff_before_output_and_error_checks()
{
    {
        std::vector<std::string> trace;
        Control control(&trace);
        Queue queue(&trace);
        Output output;
        queue.events.push_back({0, asicen::CaptureIo::Ok, nullptr, 0, 1, 0, 4096, 0});
        asicen::QueueObservation observation;
        auto zero_request = request();
        zero_request.deadline = Clock::now() + std::chrono::milliseconds(2);
        const auto result = asicen::run_queued_capture(&control, &queue, &output, zero_request, 4,
                                                       nullptr, false, nullptr, &observation);
        CHECK(result == asicen::CaptureOutcome::ZeroBytes &&
                  observation.normal_delivery_count == 1 &&
                  observation.normal_delivery_actual_bytes == 0,
              "zero-length completion is counted as a normal handoff");
    }
    {
        std::vector<std::string> trace;
        Control control(&trace);
        Queue queue(&trace);
        Output output;
        queue.events.push_back({0, asicen::CaptureIo::Error, queue.bytes.data(), 2, 1, 3, 4096, 2});
        asicen::QueueObservation observation;
        const auto result = asicen::run_queued_capture(&control, &queue, &output, request(), 4,
                                                       nullptr, false, nullptr, &observation);
        CHECK(result == asicen::CaptureOutcome::UsbFailed &&
                  observation.normal_delivery_count == 0 &&
                  observation.normal_delivery_actual_bytes == 0 &&
                  observation.callback_actual_bytes == 2 && output.bytes.empty(),
              "error callback bytes remain observable without normal handoff");
    }
    {
        std::vector<std::string> trace;
        Control control(&trace);
        Queue queue(&trace);
        Output output;
        output.accept = false;
        queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 3, 1, 0, 4096, 3});
        asicen::QueueObservation observation;
        const auto result = asicen::run_queued_capture(&control, &queue, &output, request(), 4,
                                                       nullptr, false, nullptr, &observation);
        CHECK(result == asicen::CaptureOutcome::OutputFailed &&
                  observation.normal_delivery_count == 1 &&
                  observation.normal_delivery_actual_bytes == 3 && output.bytes.empty(),
              "handoff is counted even when the downstream output rejects the data");
    }
    return true;
}

bool filter_repeat_ab_orders_lock_reset_and_restoration()
{
    for (const auto mode :
         {asicen::FilterRepeat::BeforeQueue, asicen::FilterRepeat::AfterPostStartBit}) {
        std::vector<std::string> trace;
        Control control(&trace);
        control.cf40 = 0xa8;
        control.block[0x40] = control.cf40;
        Queue queue(&trace);
        Output output;
        const auto snapshot = control.block;
        queue.events.push_back({0, asicen::CaptureIo::Ok, queue.bytes.data(), 4});
        asicen::CaptureStats stats{};
        const auto result =
            asicen::run_queued_capture(&control, &queue, &output, request(4), 4, &stats, true,
                                       &snapshot[0x40], nullptr, mode, 1, snapshot.data());
        CHECK(result == asicen::CaptureOutcome::Completed,
              "filter-repeat A/B capture completes through ordinary lifecycle");
        CHECK(control.full_block_writes == 1 && control.block == snapshot &&
                  control.restored_block == snapshot,
              "complete original CF block including zero chunks is restored");
        const auto find_after = [&](const char* value, std::size_t start) {
            for (std::size_t i = start; i < trace.size(); ++i) {
                if (trace[i] == value) {
                    return i;
                }
            }
            return trace.size();
        };
        const auto lock1 = find_after("lock", 0);
        const auto prepare = find_after("prepare", 0);
        const auto start = find_after("dsc-start", 0);
        const auto postbit = find_after(
            mode == asicen::FilterRepeat::BeforeQueue ? "cf-write:175" : "cf-write:171", start + 1);
        const auto lock2 = find_after("lock", lock1 + 1);
        const auto pulse = find_after("P", 0);
        const auto stop = find_after("dsc-stop", 0);
        const auto drain = find_after("cancel-drain", 0);
        const auto restore = find_after("block-restore", 0);
        if (mode == asicen::FilterRepeat::BeforeQueue) {
            CHECK(lock1 < pulse && pulse < prepare && prepare < start && start < postbit &&
                      postbit < lock2 && lock2 < stop,
                  "A orders pre-start lock, P, queue, DSC, post-bit lock and stop");
        } else {
            CHECK(lock1 < prepare && prepare < start && start < postbit && postbit < lock2 &&
                      lock2 < pulse && pulse < stop,
                  "B orders pre-start lock, queue, DSC, post-bit lock and P");
        }
        CHECK(stop < drain && drain < restore,
              "full CF restoration follows DSC stop and async drain");
        CHECK((control.pulse_cf40 & 0x08U) == (snapshot[0x40] & 0x08U) &&
                  (control.pulse_cf40 & 0x07U) == 0x07U,
              "repeat reset preserves the source selector bit before final restore");
    }
    return true;
}

bool filter_repeat_failure_paths_restore_full_block()
{
    const auto run_failure = [](asicen::FilterRepeat mode, std::vector<bool> locks,
                                bool pulse_failure, bool start_failure, bool expect_prepare,
                                bool expect_pulse) {
        std::vector<std::string> trace;
        Control control(&trace);
        control.lock_results = std::move(locks);
        control.fail_repeat = pulse_failure;
        control.start_result = !start_failure;
        Queue queue(&trace);
        Output output;
        const auto snapshot = control.block;
        asicen::CaptureStats stats{};
        const auto result =
            asicen::run_queued_capture(&control, &queue, &output, request(), 4, &stats, true,
                                       &snapshot[0x40], nullptr, mode, 1, snapshot.data());
        CHECK(result == asicen::CaptureOutcome::UsbFailed,
              "filter-repeat lock, P or start failure remains an error");
        CHECK(control.full_block_writes == 1 && control.block == snapshot,
              "filter-repeat failure restores the complete original block");
        CHECK((std::find(trace.begin(), trace.end(), "prepare") != trace.end()) == expect_prepare,
              "filter-repeat failure occurs on expected side of host queue setup");
        CHECK((std::find(trace.begin(), trace.end(), "P") != trace.end()) == expect_pulse,
              "filter-repeat failure invokes P only when its branch reaches it");
        return true;
    };

    CHECK(run_failure(asicen::FilterRepeat::BeforeQueue, {false}, false, false, false, false),
          "prequeue lock failure");
    CHECK(run_failure(asicen::FilterRepeat::BeforeQueue, {true}, true, false, false, true),
          "prequeue pulse failure");
    CHECK(run_failure(asicen::FilterRepeat::BeforeQueue, {true}, false, true, true, true),
          "prequeue start failure");
    CHECK(run_failure(asicen::FilterRepeat::AfterPostStartBit, {true, false}, false, false, true,
                      false),
          "post-start lock failure");
    CHECK(
        run_failure(asicen::FilterRepeat::AfterPostStartBit, {true, true}, true, false, true, true),
        "post-start pulse failure");

    {
        std::vector<std::string> trace;
        Control control(&trace);
        control.fail_cf_read_at = 1;
        control.fail_full_block_write = true;
        Queue queue(&trace);
        Output output;
        const auto snapshot = control.block;
        asicen::CaptureStats stats{};
        const auto result = asicen::run_queued_capture(
            &control, &queue, &output, request(), 4, &stats, true, &snapshot[0x40], nullptr,
            asicen::FilterRepeat::BeforeQueue, 1, snapshot.data());
        CHECK(result == asicen::CaptureOutcome::UsbFailed && stats.cf40_restore_failed,
              "early CF40 read and full-restore failures are explicitly reported");
    }
    return true;
}

}  // namespace

int main()
{
    if (!successful_order_and_limit()) {
        return 1;
    }
    if (!primary_satellite_capture_uses_lane0_endpoint_and_dsc()) {
        return 1;
    }
    if (!partial_timeout_reuses_slot_without_exceeding_depth()) {
        return 1;
    }
    if (!submit_failure_drains_partial_queue()) {
        return 1;
    }
    if (!dsc_start_failure_stops_then_drains()) {
        return 1;
    }
    if (!exact_limit_error_is_not_success()) {
        return 1;
    }
    if (!output_failure_and_cancel_drain()) {
        return 1;
    }
    if (!cancelled_and_deadline_paths_drain()) {
        return 1;
    }
    if (!resubmit_and_stop_failures_remain_errors()) {
        return 1;
    }
    if (!invalid_depth_does_not_allocate()) {
        return 1;
    }
    if (!prepare_failure_releases_without_starting()) {
        return 1;
    }
    if (!filter_start_orders_rmw_and_restores_snapshot()) {
        return 1;
    }
    if (!filter_start_restores_after_dsc_start_failure()) {
        return 1;
    }
    if (!filter_start_write_failures_restore_snapshot()) {
        return 1;
    }
    if (!cf40_response_uses_transfer_length_not_status_byte()) {
        return 1;
    }
    if (!filter_restore_failure_is_reported()) {
        return 1;
    }
    if (!queue_observation_accounts_callbacks_once_and_bounds_events()) {
        return 1;
    }
    if (!queue_observation_preserves_normal_capture_accounting()) {
        return 1;
    }
    if (!queue_observation_counts_handoff_before_output_and_error_checks()) {
        return 1;
    }
    if (!link_seed_is_applied_after_dsc_and_zeroed_after_drain()) {
        return 1;
    }
    if (!link_seed_partial_apply_and_cleanup_failures_are_reported()) {
        return 1;
    }
    if (!link_seed_is_not_cleared_if_dsc_stop_fails()) {
        return 1;
    }
    if (!link_snapshot_failure_restores_filter_snapshot()) {
        return 1;
    }
    if (!filter_repeat_ab_orders_lock_reset_and_restoration()) {
        return 1;
    }
    if (!filter_repeat_failure_paths_restore_full_block()) {
        return 1;
    }
    std::cout << "queued capture lifecycle tests passed\n";
    return 0;
}
