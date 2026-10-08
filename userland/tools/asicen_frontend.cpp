#include <libusb.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#include <unistd.h>

#include "asicen/device_profile.h"
#include "asicen/frontend_sequence.h"
#include "asicen/libusb_transport.h"
#include "asicen/protocol.h"
#include "asicen/queued_capture.h"
#include "asicen/stream_capture.h"
#include "asicen/write_protocol.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) {
    g_stop = 1;
}

struct Arguments {
    bool help = false;
    bool have_device = false;
    std::uint8_t bus = 0;
    std::uint8_t address = 0;
    bool have_port = false;
    std::string port;
    std::string command;
    bool have_local = false;
    std::uint8_t local = 1;
    bool have_reset_state = false;
    std::uint8_t reset_state = 0;
    bool have_reg = false;
    std::uint8_t reg = 0;
    bool have_length = false;
    std::uint16_t length = 1;
    bool have_frequency = false;
    std::uint32_t frequency_khz = 0;
    bool have_output = false;
    std::string output = "-";
    bool have_seconds = false;
    unsigned seconds = 5;
    bool have_packet_count = false;
    std::uint64_t packet_count = 0;
    unsigned queue_depth = 1;
    bool have_queue_depth = false;
    bool filter_start = false;
    bool shared_demod = false;
    bool have_timeout = false;
    unsigned timeout_ms = 20000;
    bool have_lock_timeout = false;
    unsigned lock_timeout_ms = 3000;
};

void usage(const char* argv0) {
    std::cerr
        << "usage:\n"
        << "  " << argv0
        << " --device BUS:ADDRESS --port BUS-PORT power-on\n"
        << "  " << argv0
        << " --device BUS:ADDRESS --port BUS-PORT restore-sibling\n"
        << "  " << argv0
        << " --device BUS:ADDRESS --port BUS-PORT [--shared-demod] init\n"
        << "  " << argv0
        << " --device BUS:ADDRESS --port BUS-PORT --frequency-khz N tune\n"
        << "  " << argv0
        << " --device BUS:ADDRESS --port BUS-PORT --frequency-khz N lock\n"
        << "  " << argv0
        << " --device BUS:ADDRESS --port BUS-PORT --frequency-khz N [--shared-demod] terrestrial\n"
        << "  " << argv0
        << " --device BUS:ADDRESS --port BUS-PORT --reset-state 0|1 [--local L] stream-setup\n"
        << "  " << argv0
        << " --device BUS:ADDRESS --port BUS-PORT --reg R [--local L] [--length N]"
           " demod-read\n"
        << "  " << argv0
        << " --device BUS:ADDRESS --port BUS-PORT --reset-state 0|1 [--local L] [--output PATH|-]"
           " [--seconds N] [--packet-count N] [--queue-depth 1|4] capture\n"
        << "\n"
        << "options: --timeout-ms N (frontend setup deadline, default 20000)\n"
        << "         --lock-timeout-ms N (post-tune lock poll, default 3000)\n"
        << "         --reset-state 0|1 (required: fourth USB_FilterReset argument)\n"
        << "         --queue-depth 1|4 (capture only; default 1, 4 queues async reads before DSC)\n"
        << "         --filter-start (capture only; local 1 + queue depth 4, restore CF40)\n"
        << "         --shared-demod (init/terrestrial only; also initialize satellite demod over I2C)\n"
        << "\n"
        << "Explicit-target frontend diagnostics. Every write verifies the fresh\n"
        << "BUS:ADDRESS, expected USB port path, VID/PID and interface 0 exclusive\n"
        << "ownership first. No hub reset, kernel-driver detach, LNB20 clear or\n"
        << "DTV_Start replay is performed.\n";
}

bool parse_uint(const std::string& text, unsigned long long max,
                unsigned long long* out) {
    if (out == nullptr || text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 0);
    if (end == nullptr || *end != '\0' || errno != 0 || parsed > max) {
        return false;
    }
    *out = parsed;
    return true;
}

bool parse_u8(const std::string& text, std::uint8_t* out) {
    unsigned long long value = 0;
    if (!parse_uint(text, 255, &value)) {
        return false;
    }
    *out = static_cast<std::uint8_t>(value);
    return true;
}

const char* kCommands[] = {"power-on",     "restore-sibling", "init",
                           "tune",         "lock",            "terrestrial",
                           "demod-read",   "capture",         "stream-setup"};

bool is_command(const std::string& value) {
    for (const char* command : kCommands) {
        if (value == command) {
            return true;
        }
    }
    return false;
}

bool parse_arguments(int argc, char** argv, Arguments* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            out->help = true;
            return true;
        }
        if (arg == "--device") {
            if (++i >= argc) {
                return false;
            }
            if (!asicen::parse_usb_location(argv[i], &out->bus, &out->address)) {
                return false;
            }
            out->have_device = true;
        } else if (arg == "--port") {
            if (++i >= argc || !asicen::parse_port_path(argv[i])) {
                return false;
            }
            out->port = argv[i];
            out->have_port = true;
        } else if (arg == "--local") {
            if (++i >= argc || !parse_u8(argv[i], &out->local) || out->local > 1) {
                return false;
            }
            out->have_local = true;
        } else if (arg == "--reset-state") {
            if (++i >= argc || !parse_u8(argv[i], &out->reset_state) ||
                out->reset_state > 1) {
                return false;
            }
            out->have_reset_state = true;
        } else if (arg == "--reg") {
            if (++i >= argc || !parse_u8(argv[i], &out->reg)) {
                return false;
            }
            out->have_reg = true;
        } else if (arg == "--length") {
            unsigned long long length = 0;
            if (++i >= argc || !parse_uint(argv[i], 0xffff, &length) ||
                length == 0) {
                return false;
            }
            out->length = static_cast<std::uint16_t>(length);
            out->have_length = true;
        } else if (arg == "--frequency-khz") {
            unsigned long long frequency = 0;
            if (++i >= argc || !parse_uint(argv[i], 0xffffffffu, &frequency) ||
                frequency == 0) {
                return false;
            }
            out->frequency_khz = static_cast<std::uint32_t>(frequency);
            out->have_frequency = true;
        } else if (arg == "--output") {
            if (++i >= argc) {
                return false;
            }
            out->output = argv[i];
            out->have_output = true;
        } else if (arg == "--seconds") {
            unsigned long long seconds = 0;
            if (++i >= argc || !parse_uint(argv[i], 3600, &seconds) ||
                seconds == 0) {
                return false;
            }
            out->seconds = static_cast<unsigned>(seconds);
            out->have_seconds = true;
        } else if (arg == "--packet-count") {
            unsigned long long packets = 0;
            if (++i >= argc || !parse_uint(argv[i], 1000000000ULL, &packets) ||
                packets == 0) {
                return false;
            }
            out->packet_count = packets;
            out->have_packet_count = true;
        } else if (arg == "--queue-depth") {
            unsigned long long depth = 0;
            if (++i >= argc || !parse_uint(argv[i], 4, &depth) ||
                (depth != 1 && depth != 4)) {
                return false;
            }
            out->queue_depth = static_cast<unsigned>(depth);
            out->have_queue_depth = true;
        } else if (arg == "--filter-start") {
            out->filter_start = true;
        } else if (arg == "--shared-demod") {
            out->shared_demod = true;
        } else if (arg == "--timeout-ms") {
            unsigned long long timeout = 0;
            if (++i >= argc || !parse_uint(argv[i], 600000, &timeout) ||
                timeout == 0) {
                return false;
            }
            out->timeout_ms = static_cast<unsigned>(timeout);
            out->have_timeout = true;
        } else if (arg == "--lock-timeout-ms") {
            unsigned long long timeout = 0;
            if (++i >= argc || !parse_uint(argv[i], 600000, &timeout) ||
                timeout == 0) {
                return false;
            }
            out->lock_timeout_ms = static_cast<unsigned>(timeout);
            out->have_lock_timeout = true;
        } else if (is_command(arg)) {
            if (!out->command.empty()) {
                return false;
            }
            out->command = arg;
        } else {
            return false;
        }
    }
    return true;
}

int remaining_ms(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
}

std::string port_path(libusb_device* device) {
    std::uint8_t ports[8]{};
    const int count = libusb_get_port_numbers(device, ports, sizeof(ports));
    if (count <= 0) {
        return "-";
    }
    std::ostringstream out;
    out << static_cast<unsigned>(libusb_get_bus_number(device)) << '-';
    for (int i = 0; i < count; ++i) {
        if (i != 0) {
            out << '.';
        }
        out << static_cast<unsigned>(ports[i]);
    }
    return out.str();
}

class LibusbFrontendTransport final : public asicen::FrontendTransport {
public:
    explicit LibusbFrontendTransport(asicen::LibusbDevice* device) : device_(device) {}

    void set_deadline(std::chrono::steady_clock::time_point deadline) {
        deadline_ = deadline;
        deadline_active_ = true;
    }

    int control(const asicen::ControlTransfer& transfer, unsigned char* data) override {
        asicen::ControlTransfer bounded = transfer;
        if (deadline_active_) {
            const int remaining = remaining_ms(deadline_);
            if (remaining <= 0) {
                return LIBUSB_ERROR_TIMEOUT;
            }
            if (bounded.timeout_ms == 0 || bounded.timeout_ms > remaining) {
                bounded.timeout_ms = static_cast<std::uint16_t>(
                    std::min<int>(remaining, 0xffff));
            }
        }
        const int rc = device_->control(bounded, data);
        // Provenance trace for the first hardware run. Frontend registers only;
        // no random-key/card/crypto material is ever transferred here.
        std::cerr << "xfer req=0x" << std::hex
                  << static_cast<unsigned>(static_cast<std::uint8_t>(bounded.request))
                  << " value=0x" << bounded.value << " index=0x" << bounded.index
                  << std::dec << " len=" << bounded.length
                  << " dir=" << (bounded.direction == asicen::Direction::In ? "in" : "out")
                  << " rc=" << rc;
        if (rc >= 1 && data != nullptr && bounded.direction == asicen::Direction::In) {
            std::cerr << " status=0x" << std::hex << static_cast<unsigned>(data[0]);
        }
        std::cerr << std::dec << '\n';
        return rc;
    }

    void delay_ms(unsigned ms) override {
        unsigned sleep_ms = ms;
        if (deadline_active_) {
            const int remaining = remaining_ms(deadline_);
            if (remaining <= 0) {
                return;
            }
            sleep_ms = std::min<unsigned>(ms, static_cast<unsigned>(remaining));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }

    bool cancelled() const override { return g_stop != 0; }

    bool expired() const override {
        return deadline_active_ && std::chrono::steady_clock::now() >= deadline_;
    }

private:
    asicen::LibusbDevice* device_;
    bool deadline_active_ = false;
    std::chrono::steady_clock::time_point deadline_{};
};

asicen::FrontendPlan concat(asicen::FrontendPlan base, const asicen::FrontendPlan& extra) {
    base.insert(base.end(), extra.begin(), extra.end());
    return base;
}

asicen::FrontendPlan terrestrial_path_plan(std::uint32_t frequency_khz,
                                           bool shared_demod) {
    asicen::FrontendPlan plan = asicen::plan_startup_subset();
    plan = concat(plan, asicen::plan_safe_power_on());
    plan = concat(plan, shared_demod
                            ? asicen::plan_terrestrial_init_with_satellite_demod()
                            : asicen::plan_terrestrial_init());
    plan = concat(plan, asicen::plan_terrestrial_tune_full(frequency_khz, 6));
    return plan;
}

const char* run_result_name(asicen::FrontendRunResult result) {
    switch (result) {
        case asicen::FrontendRunResult::Completed:
            return "completed";
        case asicen::FrontendRunResult::FailedTransfer:
            return "failed-transfer";
        case asicen::FrontendRunResult::ShortTransfer:
            return "short-transfer";
        case asicen::FrontendRunResult::InvalidArgument:
            return "invalid-argument";
        case asicen::FrontendRunResult::Cancelled:
            return "cancelled";
        case asicen::FrontendRunResult::DeadlineExceeded:
            return "deadline-exceeded";
    }
    return "unknown";
}

int verify_target(asicen::LibusbDevice* device, const Arguments& args,
                  const asicen::DeviceProfile** profile) {
    libusb_device* raw = device->device();
    if (raw == nullptr) {
        std::cerr << "selected device is not available\n";
        return 1;
    }
    libusb_device_descriptor desc{};
    const int desc_rc = libusb_get_device_descriptor(raw, &desc);
    if (desc_rc != 0) {
        std::cerr << "libusb_get_device_descriptor: " << libusb_error_name(desc_rc)
                  << '\n';
        return 1;
    }
    *profile = asicen::find_profile(desc.idVendor, desc.idProduct);
    if (*profile == nullptr) {
        std::cerr << "selected device is not a supported ASICEN runtime device\n";
        return 1;
    }
    // The recovered frontend facts are specific to the original PX-W3U3.
    if (desc.idVendor != 0x0b06 || desc.idProduct != 0x0005) {
        std::cerr << "refusing: frontend diagnostics are only verified for 0b06:0005\n";
        return 1;
    }
    const std::string actual_port = port_path(raw);
    if (actual_port != args.port) {
        std::cerr << "refusing: expected port path " << args.port << " but device is "
                  << actual_port << '\n';
        return 1;
    }
    const int driver = device->kernel_driver_active(0);
    if (driver > 0) {
        std::cerr << "refusing: kernel driver bound to interface 0\n";
        return 1;
    }
    if (driver < 0 && driver != LIBUSB_ERROR_NOT_SUPPORTED) {
        std::cerr << "kernel_driver_active: " << libusb_error_name(driver) << '\n';
        return 1;
    }
    return 0;
}

int run_plan(asicen::LibusbDevice* device, const asicen::FrontendPlan& plan,
             unsigned timeout_ms, std::uint8_t* last_read, bool* have_last_read) {
    LibusbFrontendTransport transport(device);
    transport.set_deadline(std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(timeout_ms));
    asicen::FrontendRunReport report{};
    const asicen::FrontendRunResult result =
        asicen::run_frontend_plan(plan, &transport, &report);
    std::cerr << "plan_ops=" << plan.size() << " completed=" << report.ops_completed
              << " result=" << run_result_name(result) << '\n';
    if (last_read != nullptr) {
        *last_read = report.last_read;
        *have_last_read = report.have_last_read;
    }
    if (result != asicen::FrontendRunResult::Completed) {
        std::cerr << "frontend plan stopped: " << run_result_name(result) << " after "
                  << report.ops_completed << " ops\n";
        return 1;
    }
    return 0;
}

// Post-tune lock poll with its own wall-clock timeout.
int poll_lock(asicen::LibusbDevice* device, std::uint32_t frequency_khz,
              unsigned timeout_ms, bool* locked, std::uint8_t* last_byte) {
    LibusbFrontendTransport transport(device);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    transport.set_deadline(deadline);
    *locked = false;
    while (true) {
        asicen::FrontendPlan plan = asicen::plan_terrestrial_lock_read(frequency_khz);
        if (plan.empty()) {
            std::cerr << "lock poll: frequency outside terrestrial range\n";
            return 1;
        }
        asicen::FrontendRunReport report{};
        const asicen::FrontendRunResult result =
            asicen::run_frontend_plan(plan, &transport, &report);
        if (result == asicen::FrontendRunResult::Cancelled) {
            std::cerr << "lock poll cancelled\n";
            return 1;
        }
        if (result == asicen::FrontendRunResult::DeadlineExceeded) {
            std::cerr << "lock poll: not locked before timeout\n";
            return 1;
        }
        if (result != asicen::FrontendRunResult::Completed) {
            std::cerr << "lock poll stopped: " << run_result_name(result) << '\n';
            return 1;
        }
        if (report.have_last_read) {
            *last_byte = report.last_read;
            if ((report.last_read & 0x0fU) == 0x09U) {
                *locked = true;
                return 0;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline || g_stop != 0) {
            // Do not report success for an unachieved lock.
            std::cerr << "lock poll: not locked before timeout\n";
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

class LibusbCaptureBackend final : public asicen::CaptureBackend {
public:
    explicit LibusbCaptureBackend(asicen::LibusbDevice* device) : device_(device) {}

    bool dsc_start(std::uint8_t local) override {
        unsigned char status = 0;
        return device_->control(asicen::make_dsc_control(local, false), &status) == 1;
    }

    bool dsc_stop(std::uint8_t local) override {
        unsigned char status = 0;
        return device_->control(asicen::make_dsc_control(local, true), &status) == 1;
    }

    asicen::CaptureIo bulk_read(std::uint8_t endpoint, unsigned char* data, int length,
                                int* transferred, unsigned timeout_ms) override {
        const int rc = device_->bulk_read(endpoint, data, length, transferred, timeout_ms);
        if (rc == 0) {
            return asicen::CaptureIo::Ok;
        }
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            return asicen::CaptureIo::Timeout;
        }
        return asicen::CaptureIo::Error;
    }

    bool read_cf40(std::uint8_t local, std::uint8_t* value) override {
        if (value == nullptr || local > 1) return false;
        unsigned char response[2]{};
        const int rc = device_->control(asicen::make_cf_read(local, 0x40, 1), response);
        return asicen::parse_cf40_read_response(rc, response, value);
    }

    bool write_cf40(std::uint8_t local, std::uint8_t value) override {
        if (local > 1) return false;
        asicen::ControlTransfer transfer{};
        if (!asicen::make_cf_write(local, 0x40, &value, 1, &transfer)) return false;
        unsigned char response[2]{};
        return asicen::cf40_write_response_complete(device_->control(transfer, response));
    }

    bool cancelled() const override { return g_stop != 0; }

private:
    asicen::LibusbDevice* device_;
};

class LibusbQueuedCaptureIo final : public asicen::QueuedCaptureIo {
public:
    LibusbQueuedCaptureIo(libusb_context* context, libusb_device_handle* handle)
        : context_(context), handle_(handle) {}

    ~LibusbQueuedCaptureIo() override {
        // The deadline governs capture, not ownership cleanup: cancellation is
        // drained before transfers or callback buffers can be destroyed.
        if (has_pending()) cancel_and_drain();
        release();
    }

    bool prepare(std::uint8_t endpoint, std::size_t depth,
                 std::size_t chunk_size) override {
        if (context_ == nullptr || handle_ == nullptr || endpoint == 0 ||
            depth == 0 || depth > 4 || chunk_size == 0 ||
            chunk_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        endpoint_ = endpoint;
        slots_.reserve(depth);
        for (std::size_t i = 0; i < depth; ++i) {
            auto slot = std::make_unique<Slot>();
            slot->owner = this;
            slot->index = i;
            slot->buffer.resize(chunk_size);
            slot->transfer = libusb_alloc_transfer(0);
            if (slot->transfer == nullptr) return false;
            libusb_fill_bulk_transfer(slot->transfer, handle_, endpoint_,
                                      slot->buffer.data(),
                                      static_cast<int>(slot->buffer.size()),
                                      transfer_complete, slot.get(), 0);
            slots_.push_back(std::move(slot));
        }
        return true;
    }

    bool submit(std::size_t index) override {
        Slot* slot = get_slot(index);
        if (slot == nullptr || slot->transfer == nullptr || slot->pending) return false;
        slot->pending = true;
        slot->queued = false;
        const int rc = libusb_submit_transfer(slot->transfer);
        if (rc != 0) slot->pending = false;
        return rc == 0;
    }

    asicen::QueueWait wait(unsigned timeout_ms,
                           asicen::QueueCompletion* completion) override {
        if (completion == nullptr) return asicen::QueueWait::Error;
        if (ready_.empty()) {
            timeval timeout{};
            timeout.tv_sec = static_cast<long>(timeout_ms / 1000U);
            timeout.tv_usec = static_cast<long>((timeout_ms % 1000U) * 1000U);
            const int rc = libusb_handle_events_timeout_completed(context_, &timeout,
                                                                   nullptr);
            if (rc < 0) return asicen::QueueWait::Error;
            if (ready_.empty()) return asicen::QueueWait::Timeout;
        }
        const std::size_t index = ready_.front();
        ready_.pop_front();
        Slot* slot = get_slot(index);
        if (slot == nullptr || !slot->queued) return asicen::QueueWait::Error;
        slot->queued = false;
        completion->slot = index;
        completion->io = slot->io;
        completion->data = slot->buffer.data();
        completion->size = slot->transfer->actual_length > 0
                               ? static_cast<std::size_t>(slot->transfer->actual_length)
                               : 0;
        return asicen::QueueWait::Completion;
    }

    bool resubmit(std::size_t slot) override { return submit(slot); }

    void cancel_and_drain() override {
        for (const auto& slot : slots_) {
            if (slot->pending) {
                const int rc = libusb_cancel_transfer(slot->transfer);
                (void)rc;  // NOT_FOUND means completion is already being delivered.
            }
        }
        while (has_pending()) {
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;
            const int rc = libusb_handle_events_timeout_completed(context_, &timeout,
                                                                   nullptr);
            if (rc < 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ready_.clear();
    }

    void release() override {
        if (has_pending()) return;
        for (auto& slot : slots_) {
            if (slot->transfer != nullptr) {
                libusb_free_transfer(slot->transfer);
                slot->transfer = nullptr;
            }
        }
        slots_.clear();
        ready_.clear();
    }

private:
    struct Slot {
        LibusbQueuedCaptureIo* owner = nullptr;
        std::size_t index = 0;
        libusb_transfer* transfer = nullptr;
        std::vector<unsigned char> buffer;
        asicen::CaptureIo io = asicen::CaptureIo::Error;
        bool pending = false;
        bool queued = false;
    };

    static void LIBUSB_CALL transfer_complete(libusb_transfer* transfer) {
        auto* slot = static_cast<Slot*>(transfer->user_data);
        if (slot == nullptr || slot->owner == nullptr) return;
        slot->pending = false;
        switch (transfer->status) {
            case LIBUSB_TRANSFER_COMPLETED:
                slot->io = asicen::CaptureIo::Ok;
                break;
            case LIBUSB_TRANSFER_TIMED_OUT:
                slot->io = asicen::CaptureIo::Timeout;
                break;
            default:
                slot->io = asicen::CaptureIo::Error;
                break;
        }
        slot->queued = true;
        slot->owner->ready_.push_back(slot->index);
    }

    Slot* get_slot(std::size_t index) {
        return index < slots_.size() ? slots_[index].get() : nullptr;
    }

    bool has_pending() const {
        for (const auto& slot : slots_) {
            if (slot->pending) return true;
        }
        return false;
    }

    libusb_context* context_;
    libusb_device_handle* handle_;
    std::uint8_t endpoint_ = 0;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::deque<std::size_t> ready_;
};

// Bounded, nonblocking, deadline/cancellation-aware output sink.
class PosixCaptureOutput final : public asicen::CaptureOutput {
public:
    PosixCaptureOutput(int fd, std::chrono::steady_clock::time_point deadline)
        : fd_(fd), deadline_(deadline) {}

    bool write(const unsigned char* data, std::size_t size) override {
        std::size_t offset = 0;
        while (offset < size) {
            if (g_stop != 0) {
                return false;
            }
            const int remaining = remaining_ms(deadline_);
            if (remaining <= 0) {
                return false;
            }
            const ssize_t written = ::write(fd_, data + offset, size - offset);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd descriptor{};
                descriptor.fd = fd_;
                descriptor.events = POLLOUT;
                const int polled = ::poll(&descriptor, 1, remaining);
                if (polled < 0) {
                    return false;
                }
                if (polled == 0) {
                    return false;
                }
                if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    return false;
                }
                continue;
            }
            return false;
        }
        return true;
    }

private:
    int fd_;
    std::chrono::steady_clock::time_point deadline_;
};

int run_capture(libusb_context* context, asicen::LibusbDevice* device,
                const Arguments& args) {
    const std::uint8_t endpoint = asicen::bulk_endpoint_for_lane(args.local);
    if (endpoint == 0) {
        std::cerr << "invalid local lane\n";
        return 2;
    }

    int out_fd = STDOUT_FILENO;
    int close_fd = -1;
    bool restore_flags = false;
    int saved_flags = 0;
    if (args.output != "-") {
        out_fd = ::open(args.output.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, 0644);
        if (out_fd < 0) {
            std::cerr << "cannot open output " << args.output << ": "
                      << std::strerror(errno) << '\n';
            return 1;
        }
        close_fd = out_fd;
        struct stat info {};
        if (::fstat(out_fd, &info) != 0 || S_ISDIR(info.st_mode)) {
            std::cerr << "output is not a regular file or pipe\n";
            ::close(out_fd);
            return 1;
        }
    } else {
        saved_flags = ::fcntl(out_fd, F_GETFL, 0);
        if (saved_flags < 0) {
            std::cerr << "fcntl(F_GETFL) on stdout failed: " << std::strerror(errno)
                      << '\n';
            return 1;  // before DSC start
        }
        if (::fcntl(out_fd, F_SETFL, saved_flags | O_NONBLOCK) != 0) {
            std::cerr << "fcntl(O_NONBLOCK) on stdout failed: " << std::strerror(errno)
                      << '\n';
            return 1;  // before DSC start
        }
        restore_flags = true;
    }

    auto finish_output = [&]() {
        bool close_ok = true;
        if (close_fd >= 0) {
            if (::close(close_fd) != 0) close_ok = false;
            close_fd = -1;
        }
        if (restore_flags) {
            ::fcntl(out_fd, F_SETFL, saved_flags);
            restore_flags = false;
        }
        return close_ok;
    };

    LibusbCaptureBackend backend(device);
    std::uint8_t saved_cf40 = 0;
    const std::uint8_t* cf40_to_restore = nullptr;
    if (args.filter_start && cf40_to_restore == nullptr) {
        if (!backend.read_cf40(args.local, &saved_cf40)) {
            std::cerr << "cannot snapshot CF40 before filter-start capture\n";
            finish_output();
            return 1;
        }
        cf40_to_restore = &saved_cf40;
    }

    // Capture setup resets the CF block, so the optional diagnostic snapshots
    // CF40 first and restores that original byte on every outcome.
    const int setup_result = run_plan(
        device, asicen::plan_stream_setup(args.local, args.reset_state),
        args.timeout_ms, nullptr, nullptr);
    if (setup_result != 0) {
        if (args.filter_start && cf40_to_restore != nullptr &&
            !backend.write_cf40(args.local, *cf40_to_restore)) {
            std::cerr << "CF40 restore failed after stream setup failure\n";
        }
        finish_output();
        return setup_result;
    }

    std::cerr << "RAW_UNVALIDATED: link transform is not implemented; saved bytes are "
                 "raw bulk endpoint output, not validated MPEG-TS\n";
    std::cerr << "capture lane=" << static_cast<unsigned>(args.local)
              << " endpoint=0x" << std::hex << static_cast<unsigned>(endpoint)
              << std::dec << " seconds=" << args.seconds
              << " queue_depth=" << args.queue_depth;
    if (args.filter_start) std::cerr << " filter_start=yes";
    if (args.have_packet_count) {
        std::cerr << " packet_count=" << args.packet_count;
    }
    std::cerr << '\n';

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(args.seconds);
    asicen::CaptureRequest request{};
    request.local = args.local;
    request.endpoint = endpoint;
    request.byte_limit =
        args.have_packet_count ? args.packet_count * asicen::kTsPacketBytes : 0;
    request.deadline = deadline;
    request.chunk_size = 4096;

    PosixCaptureOutput output(out_fd, deadline);
    asicen::CaptureStats stats{};
    asicen::CaptureOutcome outcome = asicen::CaptureOutcome::InvalidArgument;
    if (args.queue_depth == 4) {
        LibusbQueuedCaptureIo queued(context, device->handle());
        outcome = asicen::run_queued_capture(&backend, &queued, &output, request,
                                             args.queue_depth, &stats,
                                             args.filter_start, cf40_to_restore);
    } else {
        outcome = asicen::run_raw_capture(&backend, &output, request, &stats);
    }

    const bool close_ok = finish_output();
    if (!close_ok) std::cerr << "output close failed: " << std::strerror(errno) << '\n';

    std::cerr << "capture outcome=" << asicen::capture_outcome_name(outcome)
              << " bytes=" << stats.bytes
              << " limit_reached=" << (stats.limit_reached ? "yes" : "no")
              << " close_ok=" << (close_ok ? "yes" : "no") << '\n';
    if (stats.cf40_restore_failed) {
        std::cerr << "CF40 restore failed; device filter state may remain modified\n";
    }

    if (outcome != asicen::CaptureOutcome::Completed || !close_ok) {
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    Arguments args{};
    if (!parse_arguments(argc, argv, &args)) {
        usage(argv[0]);
        return 2;
    }
    if (args.help) {
        usage(argv[0]);
        return 0;
    }

    if (!args.have_device || !args.have_port || args.command.empty()) {
        std::cerr << "missing --device, --port or command\n";
        return 2;
    }

    // Build and validate the plan before opening USB so bad arguments never
    // mutate the device.
    asicen::FrontendPlan plan;
    bool lock_command = false;
    bool read_command = false;
    bool capture_command = false;
    bool poll_lock_after = false;
    if (args.command == "power-on") {
        plan = asicen::plan_safe_power_on();
    } else if (args.command == "restore-sibling") {
        plan = asicen::plan_sibling40_restore();
    } else if (args.command == "init") {
        plan = args.shared_demod
                   ? asicen::plan_terrestrial_init_with_satellite_demod()
                   : asicen::plan_terrestrial_init();
    } else if (args.command == "tune") {
        if (!args.have_frequency || args.frequency_khz < 40000U ||
            args.frequency_khz > 1002000U || (args.have_local && args.local != 1)) {
            std::cerr << "tune requires --frequency-khz in 40000..1002000 on local 1\n";
            return 2;
        }
        plan = asicen::plan_terrestrial_tune_full(args.frequency_khz, 6);
        poll_lock_after = true;
    } else if (args.command == "lock") {
        if (!args.have_frequency || args.frequency_khz < 40000U ||
            args.frequency_khz > 1002000U || (args.have_local && args.local != 1)) {
            std::cerr << "lock requires --frequency-khz in 40000..1002000 on local 1\n";
            return 2;
        }
        plan = asicen::plan_terrestrial_lock_read(args.frequency_khz);
        lock_command = true;
    } else if (args.command == "terrestrial") {
        if (!args.have_frequency || args.frequency_khz < 40000U ||
            args.frequency_khz > 1002000U || (args.have_local && args.local != 1)) {
            std::cerr << "terrestrial requires --frequency-khz in 40000..1002000 on "
                         "local 1\n";
            return 2;
        }
        plan = terrestrial_path_plan(args.frequency_khz, args.shared_demod);
        poll_lock_after = true;
    } else if (args.command == "demod-read") {
        if (!args.have_reg || args.length == 0 || args.length > 0x20 || args.local > 1) {
            std::cerr << "demod-read requires --reg and --length in 1..0x20\n";
            return 2;
        }
        plan = asicen::plan_demod_read(args.local, args.reg, args.length);
        read_command = true;
    } else if (args.command == "stream-setup") {
        if (args.local > 1 || !args.have_reset_state) {
            std::cerr << "stream-setup requires --reset-state 0 or 1 and --local 0 or 1\n";
            return 2;
        }
        plan = asicen::plan_stream_setup(args.local, args.reset_state);
    } else if (args.command == "capture") {
        if (!args.have_reset_state) {
            std::cerr << "capture requires --reset-state 0 or 1; source ctrl[0] is unresolved\n";
            return 2;
        }
        capture_command = true;
    }

    if (args.have_queue_depth && args.command != "capture") {
        std::cerr << "--queue-depth is only valid for capture\n";
        return 2;
    }
    if (args.filter_start &&
        (args.command != "capture" || args.local != 1 || args.queue_depth != 4)) {
        std::cerr << "--filter-start requires capture on local 1 with --queue-depth 4\n";
        return 2;
    }
    if (args.shared_demod && args.command != "init" && args.command != "terrestrial") {
        std::cerr << "--shared-demod is only valid for init or terrestrial\n";
        return 2;
    }

    if (!capture_command && plan.empty()) {
        std::cerr << "invalid arguments for command " << args.command << '\n';
        return 2;
    }

    libusb_context* context = nullptr;
    const int init_rc = libusb_init(&context);
    if (init_rc != 0) {
        std::cerr << "libusb_init: " << libusb_error_name(init_rc) << '\n';
        return 1;
    }

    asicen::LibusbDevice device;
    const asicen::UsbLocation location{args.bus, args.address};
    const int open_rc = device.open(context, location);
    if (open_rc != 0) {
        std::cerr << "open " << static_cast<unsigned>(args.bus) << ':'
                  << static_cast<unsigned>(args.address) << ": "
                  << libusb_error_name(open_rc) << '\n';
        libusb_exit(context);
        return 1;
    }

    const asicen::DeviceProfile* profile = nullptr;
    int result = verify_target(&device, args, &profile);
    if (result == 0) {
        // Claim interface 0 for exclusive ownership before any transfer.
        const int claim = device.claim_interface(0);
        if (claim < 0) {
            std::cerr << "claim interface 0: " << libusb_error_name(claim) << '\n';
            result = 1;
        }
    }

    if (result == 0) {
        asicen::write_command_summary(std::cout, std::cerr, capture_command,
                                      profile->model, args.port.c_str(), args.local);
        // Cancellation applies to setup plans as well as capture.
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        std::signal(SIGPIPE, SIG_IGN);
        if (capture_command) {
            result = run_capture(context, &device, args);
        } else {
            std::uint8_t last_read = 0;
            bool have_last_read = false;
            result = run_plan(&device, plan, args.timeout_ms, &last_read,
                              &have_last_read);
            if (result == 0 && poll_lock_after) {
                bool locked = false;
                std::uint8_t lock_byte = 0;
                result = poll_lock(&device, args.frequency_khz, args.lock_timeout_ms,
                                   &locked, &lock_byte);
                if (result == 0) {
                    std::cout << "lock_byte=0x" << std::hex
                              << static_cast<unsigned>(lock_byte) << std::dec
                              << " locked=" << (locked ? "yes" : "no") << '\n';
                }
            }
            if (result == 0 && lock_command) {
                if (!have_last_read) {
                    std::cerr << "lock register read produced no payload\n";
                    result = 1;
                } else {
                    const bool locked = (last_read & 0x0fU) == 0x09U;
                    std::cout << "lock_byte=0x" << std::hex
                              << static_cast<unsigned>(last_read) << std::dec
                              << " locked=" << (locked ? "yes" : "no") << '\n';
                }
            }
            if (result == 0 && read_command) {
                if (!have_last_read) {
                    std::cerr << "demod read produced no payload\n";
                    result = 1;
                } else {
                    std::cout << "demod_byte=0x" << std::hex
                              << static_cast<unsigned>(last_read) << std::dec << '\n';
                }
            }
        }
    }

    device.close();
    libusb_exit(context);
    return result;
}
