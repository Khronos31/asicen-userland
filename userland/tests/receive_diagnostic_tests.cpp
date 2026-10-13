// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/receive_diagnostic.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {
using asicen::ReceiveDiagnostic;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

std::string read_file(const std::string& path)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return {};
    std::string bytes;
    std::array<char, 4096> buffer{};
    std::size_t count = 0U;
    while ((count = std::fread(buffer.data(), 1U, buffer.size(), file)) != 0U) {
        bytes.append(buffer.data(), count);
    }
    std::fclose(file);
    return bytes;
}

bool exists(const std::string& path)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    std::fclose(file);
    return true;
}

bool write_sentinel(const std::string& path)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const bool written = std::fputs("keep-existing", file) >= 0;
    return std::fclose(file) == 0 && written;
}

struct Paths {
    explicit Paths(std::string value) : prefix(std::move(value)) {}
    ~Paths() { std::remove((prefix + ".raw").c_str()); std::remove((prefix + ".csv").c_str()); }
    std::string prefix;
};

bool disabled_and_invalid(const std::string& prefix)
{
    ReceiveDiagnostic recorder;
    CHECK(recorder.initialize(nullptr));
    CHECK(!recorder.enabled());
    CHECK(recorder.begin_submission(0U, 1U, 0U, 8U) == nullptr);
    recorder.submitted(nullptr, 0);
    recorder.completed(nullptr, 0, 0, true, 0U);
    recorder.decoder_input(nullptr, nullptr, 0U);
    recorder.begin_drain();
    CHECK(recorder.flush_after_drain(0U));
    CHECK(!recorder.initialize("", 8U, 2U));
    CHECK(!recorder.initialize(prefix.c_str(), 0U, 2U));
    CHECK(!recorder.initialize(prefix.c_str(), 8U, 0U));
    CHECK(!recorder.initialize(prefix.c_str(), ReceiveDiagnostic::kRawCapacity + 1U, 2U));
    CHECK(!recorder.initialize(prefix.c_str(), 8U, ReceiveDiagnostic::kRecordCapacity + 1U));
    CHECK(!exists(prefix + ".raw") && !exists(prefix + ".csv"));
    return true;
}

bool bounds_and_format(const std::string& prefix)
{
    Paths paths(prefix);
    ReceiveDiagnostic recorder;
    CHECK(recorder.initialize(prefix.c_str(), 5U, 2U));
    CHECK(recorder.enabled());
    CHECK(read_file(prefix + ".raw").empty() && read_file(prefix + ".csv").empty());
#if !defined(_WIN32)
    struct stat attributes{};
    CHECK(stat((prefix + ".raw").c_str(), &attributes) == 0);
    CHECK((attributes.st_mode & 0777) == 0600);
    CHECK(stat((prefix + ".csv").c_str(), &attributes) == 0);
    CHECK((attributes.st_mode & 0777) == 0600);
#endif
    const std::array<std::uint8_t, 8> bytes{{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}};
    auto* first = recorder.begin_submission(0U, 1U, 0U, 8U);
    auto* second = recorder.begin_submission(1U, 1U, 1U, 8U);
    CHECK(first != nullptr && second != nullptr);
    auto* dropped = recorder.begin_submission(2U, 1U, 2U, 8U);
    CHECK(dropped == nullptr);
    recorder.submitted(first, 0);
    recorder.submitted(second, 0);
    recorder.completed(second, 0, 8, true, recorder.stamp());
    recorder.completed(first, 0, 8, true, recorder.stamp());
    CHECK(first->completion_order == 2U && second->completion_order == 1U);
    recorder.decoder_input(first, bytes.data(), bytes.size());
    CHECK(first->raw_offset == 0U && first->raw_bytes == 5U && first->raw_dropped_bytes == 3U);
    CHECK(first->input_offset == 0U);
    recorder.decoder_input(second, bytes.data(), bytes.size());
    CHECK(second->raw_offset == 5U && second->raw_bytes == 0U && second->raw_dropped_bytes == 8U);
    CHECK(second->input_offset == 8U);
    recorder.completed(dropped, 0, 8, true, recorder.stamp());
    recorder.decoder_input(dropped, bytes.data(), bytes.size());
    CHECK(read_file(prefix + ".raw").empty() && read_file(prefix + ".csv").empty());
    recorder.begin_drain();
    CHECK(recorder.flush_after_drain(3U));
    CHECK(!recorder.enabled() && recorder.flush_after_drain(3U));
    CHECK(read_file(prefix + ".raw") == std::string("\0\1\2\3\4", 5U));
    const auto csv = read_file(prefix + ".csv");
    const std::string header =
        "submission_order,generation,completion_order,submit_begin_ns,submit_end_ns,"
        "callback_entry_ns,callback_ready_ns,processing_begin_ns,raw_copy_end_ns,"
        "decode_begin_ns,decode_end_ns,processing_end_ns,emit_first_ns,emit_last_ns,"
        "emit_total_ns,emit_calls,emit_accepted,input_offset,raw_offset,raw_bytes,"
        "raw_dropped_bytes,sync_loss_delta,sync_discard_delta,framer_accepted_delta,"
        "framer_emitted_delta,framer_pending_after,slot,requested_bytes,submit_result,"
        "status,actual_bytes,decode_error,disposition,callback_during_drain\n";
    CHECK(csv.rfind("# asicen_rx_diagnostic=1\n", 0U) == 0U);
    CHECK(csv.find(header) != std::string::npos);
    CHECK(csv.find("# stored_rows=2\n# dropped_records=1\n") != std::string::npos);
    CHECK(csv.find("# raw_bytes_saved=5\n# raw_bytes_omitted=19\n") != std::string::npos);
    CHECK(csv.find("# observed_valid_bytes=24\n# decoder_input_bytes=24\n") != std::string::npos);
    CHECK(csv.find("# submissions=3\n# completions=3\n") != std::string::npos);
    CHECK(csv.find("# terminal_result=3\n# finalized=1\n") != std::string::npos);
    return true;
}

bool statuses_and_no_overwrite(const std::string& prefix)
{
    {
        Paths paths(prefix + "-status");
        ReceiveDiagnostic recorder;
        CHECK(recorder.initialize(paths.prefix.c_str(), 8U, 4U));
        auto* invalid = recorder.begin_submission(0U, 1U, 0U, 8U);
        auto* drain = recorder.begin_submission(1U, 1U, 1U, 8U);
        auto* failed = recorder.begin_submission(2U, 1U, 2U, 8U);
        auto* timed_out = recorder.begin_submission(3U, 1U, 3U, 8U);
        CHECK(invalid && drain && failed && timed_out);
        recorder.submitted(invalid, 0);
        recorder.submitted(drain, 0);
        recorder.submitted(failed, -4);
        recorder.submitted(timed_out, 0);
        recorder.completed(invalid, 0, -1, false, recorder.stamp());
        CHECK(invalid->disposition == ReceiveDiagnostic::Disposition::invalid_length);
        CHECK(failed->disposition == ReceiveDiagnostic::Disposition::submit_failed);
        recorder.completed(timed_out, 2, 3, true, recorder.stamp());
        recorder.begin_drain();
        recorder.completed(drain, 3, 7, true, recorder.stamp());
        CHECK(drain->callback_during_drain && !invalid->callback_during_drain);
        CHECK(recorder.flush_after_drain(1U));
        CHECK(read_file(paths.prefix + ".raw").empty());
        const auto csv = read_file(paths.prefix + ".csv");
        CHECK(csv.find("# observed_valid_bytes=10\n# decoder_input_bytes=0\n") != std::string::npos);
        CHECK(csv.find("# invalid_lengths=1\n") != std::string::npos);
    }
    for (const char* suffix : {".raw", ".csv"}) {
        Paths paths(prefix + suffix);
        CHECK(write_sentinel(paths.prefix + suffix));
        ReceiveDiagnostic recorder;
        CHECK(!recorder.initialize(paths.prefix.c_str(), 8U, 2U));
        CHECK(!recorder.enabled());
        CHECK(read_file(paths.prefix + suffix) == "keep-existing");
        CHECK(recorder.flush_after_drain(0U));
    }
    {
        Paths paths(prefix + "-unfinished");
        { ReceiveDiagnostic recorder; CHECK(recorder.initialize(paths.prefix.c_str(), 8U, 2U)); }
        CHECK(read_file(paths.prefix + ".csv").find("# finalized=1") == std::string::npos);
    }
    return true;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string prefix = std::string(argv[1]) + "-" + std::to_string(unique);
    return disabled_and_invalid(prefix + "-disabled") && bounds_and_format(prefix + "-bounded") &&
                   statuses_and_no_overwrite(prefix + "-status") ? 0 : 1;
}
