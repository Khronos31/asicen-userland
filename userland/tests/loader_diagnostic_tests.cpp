// SPDX-License-Identifier: GPL-2.0-only
#include "loader_diagnostic.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <utility>

namespace {

namespace diagnostic = asicen::cli::loader_diagnostic;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,         \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

class Capture final {
  public:
    Capture() : file_(std::tmpfile()) {}
    ~Capture()
    {
        if (file_ != nullptr) std::fclose(file_);
    }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    FILE* file() const { return file_; }

    bool read(std::string* output)
    {
        if (file_ == nullptr || output == nullptr || std::fflush(file_) != 0 ||
            std::fseek(file_, 0, SEEK_SET) != 0) return false;
        output->clear();
        std::array<char, 4096> buffer{};
        for (;;) {
            const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), file_);
            output->append(buffer.data(), count);
            if (count < buffer.size()) break;
        }
        const bool ok = std::ferror(file_) == 0;
        std::clearerr(file_);
        return std::fseek(file_, 0, SEEK_END) == 0 && ok;
    }

  private:
    FILE* file_;
};

std::string field(const std::string& text, const char* name)
{
    const std::string prefix = std::string(name) + "=";
    std::size_t begin = 0;
    while ((begin = text.find(prefix, begin)) != std::string::npos) {
        if (begin == 0 || text[begin - 1] == ' ' || text[begin - 1] == '\n') {
            begin += prefix.size();
            const std::size_t end = text.find_first_of(" \r\n", begin);
            return text.substr(begin, end - begin);
        }
        begin += prefix.size();
    }
    return {};
}

std::string stage_line(const std::string& text, const char* stage)
{
    const std::string wanted(stage);
    std::size_t begin = 0;
    while (begin < text.size()) {
        const std::size_t end = text.find('\n', begin);
        const std::string line = text.substr(begin, end - begin);
        if (field(line, "stage") == wanted) return line;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return {};
}

std::size_t occurrences(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

bool integer_field(const std::string& text, const char* name, long long* result)
{
    const std::string value = field(text, name);
    if (value.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') return false;
    *result = parsed;
    return true;
}

diagnostic::Target known_target(diagnostic::Role role)
{
    diagnostic::Target target{};
    target.role = role;
    target.selected_index = role == diagnostic::Role::Primary ? 0 : 1;
    target.location_known = true;
    target.bus = 3;
    target.address = role == diagnostic::Role::Primary ? 17 : 29;
    target.ports[0] = 2;
    target.ports[1] = role == diagnostic::Role::Primary ? 1 : 2;
    target.port_count = 2;
    target.ids_known = true;
    target.vendor_id = 0x1738;
    target.product_id = 0x5211;
    return target;
}

struct TransferCall {
    int operator()() const { return 512; }
};

template <typename T, typename = void>
struct AcceptsPayload : std::false_type {};

template <typename T>
struct AcceptsPayload<T, std::void_t<decltype(std::declval<T&>().vendor_out(
                            std::declval<const diagnostic::Target&>(), std::size_t{},
                            std::size_t{}, std::uint8_t{}, std::uint16_t{},
                            std::uint16_t{}, std::size_t{}, std::uint16_t{},
                            std::declval<const std::uint8_t*>(), TransferCall{}))>>
    : std::true_type {};

static_assert(!AcceptsPayload<diagnostic::Trace>::value,
              "loader diagnostics must not accept raw firmware or payload bytes");

bool strict_mode_parsing()
{
    CHECK(diagnostic::parse_mode(nullptr) == diagnostic::Mode::Disabled);
    CHECK(diagnostic::parse_mode("0") == diagnostic::Mode::Disabled);
    CHECK(diagnostic::parse_mode("1") == diagnostic::Mode::Enabled);
    for (const char* value : {"", "00", "01", "10", "2", "-1", "+1", "true", "false",
                              "yes", "on", "enabled", " 1", "1 ", "\t1", "1\n", "0\n"}) {
        CHECK(diagnostic::parse_mode(value) == diagnostic::Mode::Invalid);
    }
    return true;
}

bool disabled_trace_is_inert()
{
    Capture capture;
    CHECK(capture.file() != nullptr);
    const auto target = known_target(diagnostic::Role::Sibling);
    int calls = 0;
    {
        diagnostic::Trace trace(diagnostic::Mode::Disabled, capture.file());
        CHECK(!trace.enabled());
        trace.record(diagnostic::Stage::ResolveBegin, target);
        for (const int rc : {-4, -1, 0, 511, 512, 513}) {
            const int before = calls;
            CHECK(trace.vendor_out(target, 1, 20, 0xaa, 0x1234, 0x5399, 0, 512, [&] {
                ++calls;
                return rc;
            }) == rc);
            CHECK(calls == before + 1);
        }
        trace.record(diagnostic::Stage::GpioRestoreResult, target, -4,
                     diagnostic::Domain::Libusb, true);
        CHECK(trace.finish(7) == 7);
    }
    std::string output;
    CHECK(capture.read(&output));
    CHECK(output.empty());
    CHECK(calls == 6);
    return true;
}

bool successful_calls_are_buffered_without_failure()
{
    Capture capture;
    CHECK(capture.file() != nullptr);
    const auto target = known_target(diagnostic::Role::Primary);
    int calls = 0;
    // This synthetic marker exists only inside the actual transfer callback.
    // The trace API receives numeric metadata and cannot inspect its payload.
    const std::string payload = "PRIVATE_SYNTHETIC_FIRMWARE_PAYLOAD_7e258bd1";
    std::string output;
    {
        diagnostic::Trace trace(diagnostic::Mode::Enabled, capture.file());
        CHECK(trace.enabled());
        trace.record(diagnostic::Stage::UploadBegin, target);
        for (std::size_t chunk = 1; chunk <= 20; ++chunk) {
            CHECK(trace.vendor_out(target, chunk, 20, chunk == 20 ? 0xac : 0xaa,
                                   0x1234, 0x5399, (chunk - 1) * 512, 512, [&] {
                ++calls;
                return payload.empty() ? -1 : 512;
            }) == 512);
        }
        CHECK(calls == 20);
        CHECK(trace.vendor_out(target, 1, 1, 0xac, 0, 0x5399, 0, 0, [&] {
            ++calls;
            return 0;
        }) == 0);
        trace.record(diagnostic::Stage::UploadSuccess, target);
        CHECK(capture.read(&output));
        CHECK(output.empty());
        CHECK(trace.finish(0) == 0);
        CHECK(capture.read(&output));
        CHECK(output.empty());
    }
    CHECK(calls == 21);
    CHECK(capture.read(&output));
    CHECK(!stage_line(output, "upload_begin").empty());
    CHECK(!stage_line(output, "upload_success").empty());
    CHECK(stage_line(output, "vendor_out_failure").empty());
    CHECK(output.find("failure=1") == std::string::npos);
    CHECK(output.find(payload) == std::string::npos);
    CHECK(field(output, "load_status") == "0");
    return true;
}

bool failures_preserve_result_and_transfer_metadata()
{
    struct Case {
        int rc;
        std::size_t chunk;
        diagnostic::Role role;
    };
    const Case cases[] = {{-1, 1, diagnostic::Role::Primary},
                          {-4, 20, diagnostic::Role::Sibling},
                          {511, 8, diagnostic::Role::Primary},
                          {511, 20, diagnostic::Role::Sibling},
                          {0, 20, diagnostic::Role::Sibling}};
    for (const auto& test : cases) {
        Capture capture;
        CHECK(capture.file() != nullptr);
        const auto target = known_target(test.role);
        int calls = 0;
        std::string output;
        {
            diagnostic::Trace trace(diagnostic::Mode::Enabled, capture.file());
            const int returned = trace.vendor_out(
                target, test.chunk, 20, test.chunk == 20 ? 0xac : 0xaa,
                0x1234, 0x5399, (test.chunk - 1) * 512, 512, [&] {
                    ++calls;
                    return test.rc;
                });
            CHECK(returned == test.rc);
            CHECK(calls == 1);
            CHECK(trace.finish(7) == 7);
            CHECK(capture.read(&output));
            CHECK(output.empty());
        }
        CHECK(calls == 1);
        CHECK(capture.read(&output));
        CHECK(occurrences(output, "stage=vendor_out_failure ") == 1);
        const std::string failure = stage_line(output, "vendor_out_failure");
        CHECK(field(failure, "role") ==
              (test.role == diagnostic::Role::Primary ? "primary" : "sibling"));
        CHECK(field(failure, "selected_index") == std::to_string(target.selected_index));
        CHECK(field(failure, "location_known") == "1");
        CHECK(field(failure, "bus") == "3");
        CHECK(field(failure, "address") == std::to_string(target.address));
        CHECK(field(failure, "port") ==
              (test.role == diagnostic::Role::Primary ? "2.1" : "2.2"));
        CHECK(field(failure, "ids_known") == "1");
        CHECK(field(failure, "vid") == "0x1738");
        CHECK(field(failure, "pid") == "0x5211");
        CHECK(field(failure, "chunk") == std::to_string(test.chunk));
        CHECK(field(failure, "chunks") == "20");
        CHECK(field(failure, "request") == (test.chunk == 20 ? "0xac" : "0xaa"));
        CHECK(field(failure, "value") == "0x1234");
        CHECK(field(failure, "index") == "0x5399");
        CHECK(field(failure, "blob_offset") == std::to_string((test.chunk - 1) * 512));
        CHECK(field(failure, "length") == "512");
        CHECK(field(failure, "rc") == std::to_string(test.rc));
        CHECK(field(failure, "failure") == "1");
        CHECK(field(failure, "domain") == "libusb");
        CHECK(field(failure, "error") == (test.rc == -1 ? "LIBUSB_ERROR_IO" :
                                          test.rc == -4 ? "LIBUSB_ERROR_NO_DEVICE" :
                                                          "SHORT_TRANSFER"));
        CHECK(field(output, "load_status") == "7");
        long long begin = 0;
        long long end = 0;
        CHECK(integer_field(failure, "begin_ns", &begin));
        CHECK(integer_field(failure, "end_ns", &end));
        CHECK(begin >= 0 && end >= begin);
    }
    return true;
}

bool gpio_and_unknown_identity_are_explicit()
{
    Capture capture;
    CHECK(capture.file() != nullptr);
    auto runtime = known_target(diagnostic::Role::Primary);
    runtime.address = 41;
    runtime.vendor_id = 0x0b06;
    runtime.product_id = 0x0005;
    diagnostic::Target unknown{};
    unknown.role = diagnostic::Role::Sibling;
    unknown.selected_index = 1;
    // Unknown flags must suppress stale or unobserved numeric identity values.
    unknown.bus = 99;
    unknown.address = 88;
    unknown.ports.fill(77);
    unknown.port_count = 0;
    unknown.vendor_id = 0xabcd;
    unknown.product_id = 0xef12;
    {
        diagnostic::Trace trace(diagnostic::Mode::Enabled, capture.file());
        trace.record(diagnostic::Stage::ResolveBegin, unknown);
        trace.record(diagnostic::Stage::GpioRestoreBegin, runtime);
        trace.record(diagnostic::Stage::GpioRestoreResult, runtime, -4,
                     diagnostic::Domain::Libusb, true);
        CHECK(trace.finish(7) == 7);
    }
    std::string output;
    CHECK(capture.read(&output));
    const std::string unresolved = stage_line(output, "resolve_begin");
    CHECK(field(unresolved, "role") == "sibling");
    CHECK(field(unresolved, "location_known") == "0");
    CHECK(field(unresolved, "bus") == "unknown");
    CHECK(field(unresolved, "address") == "unknown");
    CHECK(field(unresolved, "port") == "unknown");
    CHECK(field(unresolved, "ids_known") == "0");
    CHECK(field(unresolved, "vid") == "unknown");
    CHECK(field(unresolved, "pid") == "unknown");
    CHECK(unresolved.find("0xabcd") == std::string::npos);
    CHECK(unresolved.find("0xef12") == std::string::npos);
    const std::string gpio = stage_line(output, "gpio_restore_result");
    CHECK(!stage_line(output, "gpio_restore_begin").empty());
    CHECK(field(gpio, "role") == "primary");
    CHECK(field(gpio, "address") == "41");
    CHECK(field(gpio, "port") == "2.1");
    CHECK(field(gpio, "vid") == "0x0b06");
    CHECK(field(gpio, "pid") == "0x0005");
    CHECK(field(gpio, "rc") == "-4");
    CHECK(field(gpio, "failure") == "1");
    return true;
}

bool overflowing_buffer_preserves_failure()
{
    Capture capture;
    CHECK(capture.file() != nullptr);
    const auto target = known_target(diagnostic::Role::Sibling);
    std::string output;
    int calls = 0;
    {
        diagnostic::Trace trace(diagnostic::Mode::Enabled, capture.file());
        for (std::size_t i = 0; i < diagnostic::Trace::kEventCapacity * 4; ++i)
            trace.record(diagnostic::Stage::ResolveBegin, target);
        CHECK(trace.vendor_out(target, 20, 20, 0xac, 0x4567, 0x5399, 9728, 512, [&] {
            ++calls;
            return -4;
        }) == -4);
        // A later phase/error summary cannot displace the actual USB failure.
        trace.record(diagnostic::Stage::LoadResult, target, 7,
                     diagnostic::Domain::ExitStatus, true);
        CHECK(trace.finish(7) == 7);
        CHECK(capture.read(&output));
        CHECK(output.empty());
    }
    CHECK(calls == 1);
    CHECK(capture.read(&output));
    CHECK(output.size() < 128 * 1024);
    CHECK(occurrences(output, "\n") <= diagnostic::Trace::kEventCapacity + 8);
    long long buffered = 0;
    long long dropped = 0;
    CHECK(integer_field(output, "buffered", &buffered));
    CHECK(integer_field(output, "dropped", &dropped));
    CHECK(buffered <= static_cast<long long>(diagnostic::Trace::kEventCapacity));
    CHECK(dropped > 0);
    const std::string failure = stage_line(output, "vendor_out_failure");
    CHECK(field(failure, "rc") == "-4");
    CHECK(field(failure, "chunk") == "20");
    CHECK(field(failure, "value") == "0x4567");
    CHECK(field(failure, "address") == "29");
    CHECK(field(output, "load_status") == "7");
    return true;
}

bool identity_snapshots_and_port_bounds_are_preserved()
{
    Capture capture;
    CHECK(capture.file() != nullptr);
    auto target = known_target(diagnostic::Role::Primary);
    {
        diagnostic::Trace trace(diagnostic::Mode::Enabled, capture.file());
        trace.record(diagnostic::Stage::OpenBegin, target);
        target.address = 42;
        target.ports = {{1, 2, 3, 4, 5, 6, 7, 8}};
        target.port_count = 8;
        trace.record(diagnostic::Stage::OpenResult, target, 0, diagnostic::Domain::Libusb);
        target.port_count = 255;
        trace.record(diagnostic::Stage::TopologyResult, target);
        target.port_count = 0;
        target.ports.fill(99);
        target.address = 99;
        CHECK(trace.finish(0) == 0);
    }
    std::string output;
    CHECK(capture.read(&output));
    const std::string before = stage_line(output, "open_begin");
    const std::string after = stage_line(output, "open_result");
    CHECK(field(before, "address") == "17");
    CHECK(field(before, "port") == "2.1");
    CHECK(field(after, "address") == "42");
    CHECK(field(after, "port") == "1.2.3.4.5.6.7.8");
    CHECK(field(after, "error") == "LIBUSB_SUCCESS");
    CHECK(field(after, "failure") == "0");
    CHECK(field(stage_line(output, "topology_result"), "port") == "unknown");
    long long before_sequence = 0;
    long long after_sequence = 0;
    CHECK(integer_field(before, "seq", &before_sequence));
    CHECK(integer_field(after, "seq", &after_sequence));
    CHECK(before_sequence < after_sequence);
    return true;
}

bool clock_anchors_are_labelled_without_assuming_dmesg_epoch()
{
    Capture capture;
    CHECK(capture.file() != nullptr);
    {
        diagnostic::Trace trace(diagnostic::Mode::Enabled, capture.file());
        CHECK(trace.finish(0) == 0);
    }
    std::string output;
    CHECK(capture.read(&output));
#ifdef __linux__
    CHECK(field(output, "platform") == "linux");
    CHECK(field(output, "elapsed_clock") == "CLOCK_MONOTONIC");
    CHECK(field(output, "wall_clock") == "CLOCK_REALTIME");
    CHECK(field(output, "boottime_clock") == "CLOCK_BOOTTIME");
#else
    CHECK(field(output, "platform") == "portable");
    CHECK(field(output, "elapsed_clock") == "steady_clock");
    CHECK(field(output, "wall_clock") == "system_clock");
    CHECK(field(output, "boottime_clock") == "unknown");
#endif
    for (const char* name : {"wall_ns", "monotonic_ns", "boottime_ns"}) {
        long long value = 0;
        CHECK(integer_field(output, name, &value));
        CHECK(value >= -1);
    }
    long long monotonic = 0;
    long long pair_end = 0;
    long long pair_span = 0;
    CHECK(integer_field(output, "monotonic_ns", &monotonic));
    CHECK(integer_field(output, "pair_end_monotonic_ns", &pair_end));
    CHECK(integer_field(output, "pair_span_ns", &pair_span));
    if (monotonic >= 0 && pair_end >= 0) {
        CHECK(pair_end >= monotonic);
        CHECK(pair_span == pair_end - monotonic);
    } else {
        CHECK(pair_span == -1);
    }
    for (const char* name : {"timens_monotonic_known", "timens_boottime_known"}) {
        CHECK(field(output, name) == "0" || field(output, name) == "1");
    }
    const std::string scope_verified = field(output, "timens_scope_verified");
    CHECK(scope_verified == "0" || scope_verified == "1");
    if (scope_verified == "0") {
        CHECK(field(output, "timens_monotonic_known") == "0");
        CHECK(field(output, "timens_boottime_known") == "0");
    }
    for (const char* name : {"timens_monotonic_sec", "timens_boottime_sec"}) {
        long long value = 0;
        CHECK(integer_field(output, name, &value));
    }
    for (const char* name : {"timens_monotonic_nsec", "timens_boottime_nsec"}) {
        long long value = 0;
        CHECK(integer_field(output, name, &value));
        CHECK(value >= 0 && value < 1000000000LL);
    }
#ifndef __linux__
    CHECK(field(output, "boottime_ns") == "-1");
    CHECK(scope_verified == "0");
    CHECK(field(output, "timens_monotonic_known") == "0");
    CHECK(field(output, "timens_boottime_known") == "0");
#endif
    CHECK(field(output, "pair_atomic") == "0");
    CHECK(field(output, "timing_may_be_perturbed") == "1");
    CHECK(field(output, "dmesg_epoch_assumed") == "0");
    return true;
}

bool warm_loader_status_is_not_a_process_exit_error()
{
    Capture capture;
    CHECK(capture.file() != nullptr);
    std::string output;
    {
        diagnostic::Trace trace(diagnostic::Mode::Enabled, capture.file());
        CHECK(trace.finish(1) == 1);
        CHECK(capture.read(&output));
        CHECK(output.empty());
    }
    CHECK(capture.read(&output));
    const std::string result = stage_line(output, "load_result");
    CHECK(field(result, "domain") == "loader_status");
    CHECK(field(result, "rc") == "1");
    CHECK(field(result, "error") == "ALREADY_RUNTIME");
    CHECK(field(result, "failure") == "0");
    CHECK(field(output, "load_status") == "1");
    CHECK(stage_line(output, "vendor_out_failure").empty());
    return true;
}

bool null_sink_does_not_change_transfer_or_status()
{
    int calls = 0;
    diagnostic::Trace trace(diagnostic::Mode::Enabled, nullptr);
    CHECK(trace.vendor_out(known_target(diagnostic::Role::Primary), 1, 20,
                           0xaa, 0, 0x5399, 0, 512, [&] {
        ++calls;
        return -1;
    }) == -1);
    CHECK(calls == 1);
    CHECK(trace.finish(7) == 7);
    return true;
}

} // namespace

int main()
{
    if (!strict_mode_parsing() || !disabled_trace_is_inert() ||
        !successful_calls_are_buffered_without_failure() ||
        !failures_preserve_result_and_transfer_metadata() ||
        !gpio_and_unknown_identity_are_explicit() || !overflowing_buffer_preserves_failure() ||
        !identity_snapshots_and_port_bounds_are_preserved() ||
        !clock_anchors_are_labelled_without_assuming_dmesg_epoch() ||
        !warm_loader_status_is_not_a_process_exit_error() ||
        !null_sink_does_not_change_transfer_or_status()) return 1;
    return 0;
}
