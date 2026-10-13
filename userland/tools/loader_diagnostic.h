// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#if defined(__linux__)
#include <sys/stat.h>
#include <time.h>
#endif

namespace asicen::cli::loader_diagnostic {

enum class Mode { Disabled, Enabled, Invalid };

inline Mode parse_mode(const char* value) noexcept
{
    if (value == nullptr || std::strcmp(value, "0") == 0) return Mode::Disabled;
    if (std::strcmp(value, "1") == 0) return Mode::Enabled;
    return Mode::Invalid;
}

enum class Role { None, Primary, Sibling };
enum class Domain { None, Libusb, Px4, ExitStatus, LoaderStatus };
enum class Stage {
    ResolveBegin, ResolveResult, OpenBegin, OpenResult, DescriptorResult,
    TopologyResult, KernelDriverResult, ClaimBegin, ClaimResult, UploadBegin,
    UploadSuccess, VendorOutFailure, PrimaryRuntimeBegin, PrimaryRuntimeFound,
    PrimaryRuntimeResult, RuntimeOpenBegin, RuntimeOpenResult, RuntimeClaimBegin,
    RuntimeClaimResult, GpioRestoreBegin, GpioRestoreResult, LoadResult
};

// All identity fields are snapshots taken before a transfer. Unknown fields
// remain explicitly unknown; no USB requests are made by this helper.
struct Target {
    Role role = Role::None;
    std::size_t selected_index = 0U;
    bool location_known = false;
    std::uint8_t bus = 0U;
    std::uint8_t address = 0U;
    std::array<std::uint8_t, 8> ports{};
    std::uint8_t port_count = 0U;
    bool ids_known = false;
    std::uint16_t vendor_id = 0U;
    std::uint16_t product_id = 0U;
};

class Trace final {
public:
    static constexpr std::size_t kEventCapacity = 64U;

    explicit Trace(Mode mode, std::FILE* sink = stderr) noexcept
        : enabled_(mode == Mode::Enabled), sink_(sink)
    {
        if (!enabled_) return;
        // These samples are deliberately reported as a non-atomic pair.
#if defined(__linux__)
        monotonic_ns_ = linux_clock_ns(CLOCK_MONOTONIC);
        wall_ns_ = linux_clock_ns(CLOCK_REALTIME);
        boottime_ns_ = linux_clock_ns(CLOCK_BOOTTIME);
        pair_end_ns_ = linux_clock_ns(CLOCK_MONOTONIC);
        read_time_namespace_offsets();
#else
        monotonic_ns_ = monotonic_now();
        wall_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        pair_end_ns_ = monotonic_now();
#endif
    }

    ~Trace() noexcept { emit(); }
    Trace(const Trace&) = delete;
    Trace& operator=(const Trace&) = delete;

    bool enabled() const noexcept { return enabled_; }

    void record(Stage stage, const Target& target, int rc = 0,
                Domain domain = Domain::None, bool failure = false) noexcept
    {
        if (!enabled_) return;
        Event event{};
        event.stage = stage;
        event.target = target;
        event.rc = rc;
        event.domain = domain;
        event.failure = failure;
        event.begin_ns = elapsed_now();
        event.end_ns = event.begin_ns;
        append(event);
    }

    // Call the original operation exactly once, with no clock or output work
    // when disabled. Successful chunks are never buffered or printed.
    template<class Function>
    int vendor_out(const Target& target, std::size_t chunk, std::size_t total,
                   std::uint8_t request, std::uint16_t value,
                   std::uint16_t index, std::size_t offset,
                   std::uint16_t length, Function&& call)
    {
        if (!enabled_) return std::forward<Function>(call)();
        const std::int64_t begin = elapsed_now();
        const int rc = std::forward<Function>(call)();
        const std::int64_t end = elapsed_now();
        if (rc < 0 || rc != length) {
            Event event{};
            event.stage = Stage::VendorOutFailure;
            event.target = target;
            event.rc = rc;
            event.domain = Domain::Libusb;
            event.failure = true;
            event.begin_ns = begin;
            event.end_ns = end;
            event.chunk = chunk;
            event.total = total;
            event.request = request;
            event.value = value;
            event.index = index;
            event.offset = offset;
            event.length = length;
            append(event);
        }
        return rc;
    }

    int finish(int status) noexcept
    {
        if (enabled_) {
            load_status_ = status;
            record(Stage::LoadResult, Target{}, status, Domain::LoaderStatus);
        }
        return status;
    }

private:
    struct Event {
        Stage stage = Stage::LoadResult;
        Target target{};
        int rc = 0;
        Domain domain = Domain::None;
        bool failure = false;
        std::int64_t begin_ns = -1;
        std::int64_t end_ns = -1;
        std::size_t sequence = 0U;
        std::size_t chunk = 0U;
        std::size_t total = 0U;
        std::uint8_t request = 0U;
        std::uint16_t value = 0U;
        std::uint16_t index = 0U;
        std::size_t offset = 0U;
        std::uint16_t length = 0U;
    };

    struct Offset {
        bool known = false;
        long long seconds = 0;
        long long nanoseconds = 0;
    };

#if defined(__linux__)
    static std::int64_t linux_clock_ns(clockid_t clock) noexcept
    {
        timespec sample{};
        if (::clock_gettime(clock, &sample) != 0) return -1;
        return static_cast<std::int64_t>(sample.tv_sec) * 1000000000LL +
               static_cast<std::int64_t>(sample.tv_nsec);
    }

    void read_time_namespace_offsets() noexcept
    {
        // timens_offsets describes time_for_children, not necessarily the
        // active time namespace. Accept it only when both nsfs identities
        // match, and remain unchanged across this read.
        struct stat active{};
        struct stat children{};
        if (::stat("/proc/self/ns/time", &active) != 0 ||
            ::stat("/proc/self/ns/time_for_children", &children) != 0 ||
            active.st_dev != children.st_dev || active.st_ino != children.st_ino) return;
        std::FILE* file = std::fopen("/proc/self/timens_offsets", "r");
        if (file == nullptr) return;
        Offset monotonic{};
        Offset boottime{};
        char line[128]{};
        for (unsigned int i = 0U; i < 4U && std::fgets(line, sizeof(line), file); ++i) {
            char name[16]{};
            long long seconds = 0;
            long long nanoseconds = 0;
            char extra = '\0';
            if (std::sscanf(line, "%15s %lld %lld %c", name, &seconds,
                            &nanoseconds, &extra) != 3) continue;
            if (nanoseconds < 0 || nanoseconds >= 1000000000LL) continue;
            if (std::strcmp(name, "monotonic") == 0)
                monotonic = Offset{true, seconds, nanoseconds};
            else if (std::strcmp(name, "boottime") == 0)
                boottime = Offset{true, seconds, nanoseconds};
        }
        const bool read_ok = std::ferror(file) == 0;
        std::fclose(file);
        struct stat active_after{};
        struct stat children_after{};
        if (!read_ok || ::stat("/proc/self/ns/time", &active_after) != 0 ||
            ::stat("/proc/self/ns/time_for_children", &children_after) != 0 ||
            active_after.st_dev != active.st_dev || active_after.st_ino != active.st_ino ||
            children_after.st_dev != active.st_dev || children_after.st_ino != active.st_ino) return;
        time_namespace_verified_ = true;
        monotonic_offset_ = monotonic;
        boottime_offset_ = boottime;
    }
#endif

    static std::int64_t monotonic_now() noexcept
    {
#if defined(__linux__)
        return linux_clock_ns(CLOCK_MONOTONIC);
#else
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
    }

    std::int64_t elapsed_now() const noexcept
    {
        const std::int64_t now = monotonic_now();
        return now < 0 || monotonic_ns_ < 0 ? -1 : now - monotonic_ns_;
    }

    void append(Event event) noexcept
    {
        event.sequence = sequence_++;
        if (count_ < events_.size()) {
            events_[count_++] = event;
        } else {
            ++dropped_;
            // A later failure must remain visible even if phase events filled
            // the buffer. Each upload stops at its first failed chunk.
            if (event.failure && (!have_overflow_failure_ ||
                overflow_failure_.stage != Stage::VendorOutFailure ||
                event.stage == Stage::VendorOutFailure)) {
                overflow_failure_ = event;
                have_overflow_failure_ = true;
            }
        }
    }

    static const char* stage_name(Stage stage) noexcept
    {
        switch (stage) {
        case Stage::ResolveBegin: return "resolve_begin";
        case Stage::ResolveResult: return "resolve_result";
        case Stage::OpenBegin: return "open_begin";
        case Stage::OpenResult: return "open_result";
        case Stage::DescriptorResult: return "descriptor_result";
        case Stage::TopologyResult: return "topology_result";
        case Stage::KernelDriverResult: return "kernel_driver_result";
        case Stage::ClaimBegin: return "claim_begin";
        case Stage::ClaimResult: return "claim_result";
        case Stage::UploadBegin: return "upload_begin";
        case Stage::UploadSuccess: return "upload_success";
        case Stage::VendorOutFailure: return "vendor_out_failure";
        case Stage::PrimaryRuntimeBegin: return "primary_runtime_begin";
        case Stage::PrimaryRuntimeFound: return "primary_runtime_found";
        case Stage::PrimaryRuntimeResult: return "primary_runtime_result";
        case Stage::RuntimeOpenBegin: return "runtime_open_begin";
        case Stage::RuntimeOpenResult: return "runtime_open_result";
        case Stage::RuntimeClaimBegin: return "runtime_claim_begin";
        case Stage::RuntimeClaimResult: return "runtime_claim_result";
        case Stage::GpioRestoreBegin: return "gpio_restore_begin";
        case Stage::GpioRestoreResult: return "gpio_restore_result";
        case Stage::LoadResult: return "load_result";
        }
        return "unknown";
    }

    static const char* error_name(const Event& event) noexcept
    {
        if (event.domain == Domain::None) return "none";
        if (event.domain == Domain::LoaderStatus) {
            if (event.rc == 1) return "ALREADY_RUNTIME";
            return event.rc == 0 ? "OK" : "STATUS";
        }
        if (event.domain == Domain::ExitStatus) return event.rc == 0 ? "OK" : "STATUS";
        if (event.domain == Domain::Px4) return event.rc == 0 ? "PX4_OK" : "PX4_ERROR";
        if (event.stage == Stage::VendorOutFailure && event.rc >= 0) return "SHORT_TRANSFER";
        if (event.failure && event.rc >= 0) return "SHORT_OR_FAILED_RESULT";
        switch (event.rc) {
        case 0: return "LIBUSB_SUCCESS";
        case -1: return "LIBUSB_ERROR_IO";
        case -2: return "LIBUSB_ERROR_INVALID_PARAM";
        case -3: return "LIBUSB_ERROR_ACCESS";
        case -4: return "LIBUSB_ERROR_NO_DEVICE";
        case -5: return "LIBUSB_ERROR_NOT_FOUND";
        case -6: return "LIBUSB_ERROR_BUSY";
        case -7: return "LIBUSB_ERROR_TIMEOUT";
        case -8: return "LIBUSB_ERROR_OVERFLOW";
        case -9: return "LIBUSB_ERROR_PIPE";
        case -10: return "LIBUSB_ERROR_INTERRUPTED";
        case -11: return "LIBUSB_ERROR_NO_MEM";
        case -12: return "LIBUSB_ERROR_NOT_SUPPORTED";
        case -99: return "LIBUSB_ERROR_OTHER";
        default: return event.rc > 0 ? "LIBUSB_SUCCESS" : "LIBUSB_ERROR_UNKNOWN";
        }
    }

    void emit_event(const Event& event, bool retained) const noexcept
    {
        const Target& target = event.target;
        const char* role = target.role == Role::Primary ? "primary" :
                           target.role == Role::Sibling ? "sibling" : "none";
        const char* domain = event.domain == Domain::Libusb ? "libusb" :
                             event.domain == Domain::Px4 ? "px4" :
                             event.domain == Domain::LoaderStatus ? "loader_status" :
                             event.domain == Domain::ExitStatus ? "exit_status" : "none";
        std::fprintf(sink_, "loader-diag stage=%s seq=%zu role=%s selected_index=%zu"
                     " begin_ns=%lld end_ns=%lld location_known=%u",
                     stage_name(event.stage), event.sequence, role, target.selected_index,
                     static_cast<long long>(event.begin_ns), static_cast<long long>(event.end_ns),
                     target.location_known ? 1U : 0U);
        if (target.location_known)
            std::fprintf(sink_, " bus=%u address=%u", static_cast<unsigned int>(target.bus),
                         static_cast<unsigned int>(target.address));
        else
            std::fputs(" bus=unknown address=unknown", sink_);
        std::fputs(" port=", sink_);
        if (target.port_count == 0U || target.port_count > target.ports.size()) {
            std::fputs("unknown", sink_);
        } else {
            for (std::size_t i = 0U; i < target.port_count; ++i)
                std::fprintf(sink_, "%s%u", i == 0U ? "" : ".",
                             static_cast<unsigned int>(target.ports[i]));
        }
        std::fprintf(sink_, " ids_known=%u", target.ids_known ? 1U : 0U);
        if (target.ids_known)
            std::fprintf(sink_, " vid=0x%04x pid=0x%04x", static_cast<unsigned int>(target.vendor_id),
                         static_cast<unsigned int>(target.product_id));
        else
            std::fputs(" vid=unknown pid=unknown", sink_);
        std::fprintf(sink_, " domain=%s rc=%d error=%s failure=%u overflow_retained=%u",
                     domain, event.rc,
                     error_name(event), event.failure ? 1U : 0U, retained ? 1U : 0U);
        if (event.stage == Stage::VendorOutFailure)
            std::fprintf(sink_, " chunk=%zu chunks=%zu request=0x%02x value=0x%04x"
                         " index=0x%04x blob_offset=%zu length=%u",
                         event.chunk, event.total, static_cast<unsigned int>(event.request),
                         static_cast<unsigned int>(event.value), static_cast<unsigned int>(event.index),
                         event.offset, static_cast<unsigned int>(event.length));
        std::fputc('\n', sink_);
    }

    void emit() const noexcept
    {
        if (!enabled_ || sink_ == nullptr) return;
#if defined(__linux__)
        const char* platform = "linux";
        const char* elapsed_clock = "CLOCK_MONOTONIC";
        const char* wall_clock = "CLOCK_REALTIME";
        const char* boottime_clock = "CLOCK_BOOTTIME";
#else
        const char* platform = "portable";
        const char* elapsed_clock = "steady_clock";
        const char* wall_clock = "system_clock";
        const char* boottime_clock = "unknown";
#endif
        std::fprintf(sink_, "loader-diag anchor platform=%s elapsed_clock=%s wall_clock=%s"
                     " boottime_clock=%s"
                     " wall_ns=%lld monotonic_ns=%lld boottime_ns=%lld pair_atomic=0"
                     " pair_end_monotonic_ns=%lld pair_span_ns=%lld"
                     " timens_scope_verified=%u"
                     " timens_monotonic_known=%u timens_monotonic_sec=%lld timens_monotonic_nsec=%lld"
                     " timens_boottime_known=%u timens_boottime_sec=%lld timens_boottime_nsec=%lld"
                     " timing_may_be_perturbed=1 dmesg_epoch_assumed=0\n",
                     platform, elapsed_clock, wall_clock, boottime_clock, static_cast<long long>(wall_ns_),
                     static_cast<long long>(monotonic_ns_), static_cast<long long>(boottime_ns_),
                     static_cast<long long>(pair_end_ns_),
                     static_cast<long long>(monotonic_ns_ < 0 || pair_end_ns_ < 0 ? -1 :
                                            pair_end_ns_ - monotonic_ns_),
                     time_namespace_verified_ ? 1U : 0U,
                     monotonic_offset_.known ? 1U : 0U, monotonic_offset_.seconds,
                     monotonic_offset_.nanoseconds, boottime_offset_.known ? 1U : 0U,
                     boottime_offset_.seconds, boottime_offset_.nanoseconds);
        for (std::size_t i = 0U; i < count_; ++i) emit_event(events_[i], false);
        if (have_overflow_failure_) emit_event(overflow_failure_, true);
        std::fprintf(sink_, "loader-diag summary buffered=%zu dropped=%zu"
                     " retained_failure=%u load_status=%d\n", count_, dropped_,
                     have_overflow_failure_ ? 1U : 0U, load_status_);
    }

    bool enabled_ = false;
    std::FILE* sink_ = nullptr;
    std::int64_t wall_ns_ = -1;
    std::int64_t monotonic_ns_ = -1;
    std::int64_t boottime_ns_ = -1;
    std::int64_t pair_end_ns_ = -1;
    Offset monotonic_offset_{};
    Offset boottime_offset_{};
    bool time_namespace_verified_ = false;
    std::array<Event, kEventCapacity> events_{};
    std::size_t count_ = 0U;
    std::size_t sequence_ = 0U;
    std::size_t dropped_ = 0U;
    Event overflow_failure_{};
    bool have_overflow_failure_ = false;
    int load_status_ = -1;
};

}  // namespace asicen::cli::loader_diagnostic
