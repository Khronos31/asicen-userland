// SPDX-License-Identifier: GPL-2.0-only
#ifndef ASICEN_USERLAND_RECEIVE_DIAGNOSTIC_H
#define ASICEN_USERLAND_RECEIVE_DIAGNOSTIC_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace asicen {

// A bounded, diagnostic-only host observation. The caller serializes submit and
// callback updates with the existing queue lock and flushes only after draining.
// Callback timestamps do not measure USB bus arrival time. No payload other than
// the exact bulk input passed to the transport decoder belongs in this recorder.
class ReceiveDiagnostic final {
public:
    static constexpr std::size_t kRawCapacity = 64U * 1024U * 1024U;
    static constexpr std::size_t kRecordCapacity = 65536U;
    enum class Disposition : unsigned {
        submitted = 0U,
        not_decoded = 1U,
        decoded = 2U,
        no_data = 3U,
        status_error = 4U,
        invalid_length = 5U,
        submit_failed = 6U,
    };
    struct Record {
        std::uint64_t submission_order = 0U;
        std::uint64_t generation = 0U;
        std::uint64_t completion_order = 0U; // One-based; zero means no callback.
        std::uint64_t submit_begin_ns = 0U;
        std::uint64_t submit_end_ns = 0U;
        std::uint64_t callback_entry_ns = 0U;
        std::uint64_t callback_ready_ns = 0U;
        std::uint64_t processing_begin_ns = 0U;
        std::uint64_t raw_copy_end_ns = 0U;
        std::uint64_t decode_begin_ns = 0U;
        std::uint64_t decode_end_ns = 0U;
        std::uint64_t processing_end_ns = 0U;
        std::uint64_t emit_first_ns = 0U;
        std::uint64_t emit_last_ns = 0U;
        std::uint64_t emit_total_ns = 0U;
        std::uint64_t emit_calls = 0U;
        std::uint64_t emit_accepted = 0U;
        std::uint64_t input_offset = 0U;
        std::uint64_t raw_offset = 0U;
        std::uint64_t raw_bytes = 0U;
        std::uint64_t raw_dropped_bytes = 0U;
        std::uint64_t sync_loss_delta = 0U;
        std::uint64_t sync_discard_delta = 0U;
        std::uint64_t framer_accepted_delta = 0U;
        std::uint64_t framer_emitted_delta = 0U;
        std::uint64_t framer_pending_after = 0U;
        std::uint32_t slot = 0U;
        std::uint32_t requested_bytes = 0U;
        int submit_result = 0;
        int status = -1;
        int actual_bytes = 0;
        int decode_error = 0;
        Disposition disposition = Disposition::submitted;
        bool callback_during_drain = false;
    };

    ReceiveDiagnostic() noexcept = default;
    ~ReceiveDiagnostic() noexcept;
    ReceiveDiagnostic(const ReceiveDiagnostic&) = delete;
    ReceiveDiagnostic& operator=(const ReceiveDiagnostic&) = delete;

    // A null prefix disables capture; an empty prefix is an error. Existing
    // files are never overwritten. Smaller limits support offline boundary tests.
    bool initialize(const char* prefix, std::size_t raw_capacity = kRawCapacity,
                    std::size_t record_capacity = kRecordCapacity) noexcept;
    bool enabled() const noexcept { return enabled_; }
    std::uint64_t stamp() const noexcept;
    Record* begin_submission(std::uint32_t slot, std::uint64_t generation,
                             std::uint64_t order, std::uint32_t requested) noexcept;
    void submitted(Record*, int result) noexcept;
    void completed(Record*, int status, int actual, bool valid_length,
                   std::uint64_t callback_entry_ns) noexcept;
    void decoder_input(Record*, const std::uint8_t*, std::size_t) noexcept;
    void begin_drain() noexcept { draining_ = true; }
    // Must never be called while callbacks can still access a record.
    bool flush_after_drain(unsigned terminal_result) noexcept;

private:
    bool close_files() noexcept;
    std::unique_ptr<std::uint8_t[]> raw_;
    std::unique_ptr<Record[]> records_;
    std::FILE* raw_file_ = nullptr;
    std::FILE* csv_file_ = nullptr;
    std::chrono::steady_clock::time_point epoch_{};
    std::size_t raw_capacity_ = 0U;
    std::size_t record_capacity_ = 0U;
    std::size_t raw_size_ = 0U;
    std::size_t record_count_ = 0U;
    std::uint64_t submissions_ = 0U;
    std::uint64_t completions_ = 0U;
    std::uint64_t observed_valid_bytes_ = 0U;
    std::uint64_t invalid_lengths_ = 0U;
    std::uint64_t decoder_bytes_ = 0U;
    std::uint64_t dropped_records_ = 0U;
    std::uint64_t dropped_raw_bytes_ = 0U;
    bool enabled_ = false;
    bool draining_ = false;
};

// Platform-specific exclusive creation; POSIX files are mode 0600 and Windows
// files use a protected current-user-only DACL. No global umask/ACL is changed.
std::FILE* open_receive_diagnostic_file(const char* path) noexcept;

}  // namespace asicen
#endif
