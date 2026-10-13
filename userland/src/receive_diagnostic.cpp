// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/receive_diagnostic.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <new>

namespace asicen {
namespace {
void touch_buffer(void* data, std::size_t size) noexcept
{
    // Force writes before USB starts even if the allocator supplies lazy zero
    // pages. This reduces first-use page faults; it does not pin memory or claim
    // that later faults/scheduling overhead are impossible.
    auto* bytes = static_cast<volatile unsigned char*>(data);
    for (std::size_t offset = 0U; offset < size; offset += 4096U) {
        bytes[offset] = bytes[offset];
    }
    bytes[size - 1U] = bytes[size - 1U];
}
}  // namespace

ReceiveDiagnostic::~ReceiveDiagnostic() noexcept
{
    // An undrained recorder is quarantined with AsyncState, never destroyed.
    // Destruction is not an alternative to the explicit drained flush.
    (void)close_files();
}

bool ReceiveDiagnostic::close_files() noexcept
{
    bool ok = true;
    if (raw_file_ != nullptr) {
        ok = std::fclose(raw_file_) == 0 && ok;
        raw_file_ = nullptr;
    }
    if (csv_file_ != nullptr) {
        ok = std::fclose(csv_file_) == 0 && ok;
        csv_file_ = nullptr;
    }
    return ok;
}

bool ReceiveDiagnostic::initialize(const char* prefix, std::size_t raw_capacity,
                                    std::size_t record_capacity) noexcept
{
    if (enabled_) {
        std::fprintf(stderr, "receive diagnostic: recorder is already active\n");
        return false;
    }
    if (prefix == nullptr) {
        return true;
    }
    char raw_path[4096]{};
    char csv_path[4096]{};
    const int raw_length = std::snprintf(raw_path, sizeof(raw_path), "%s.raw", prefix);
    const int csv_length = std::snprintf(csv_path, sizeof(csv_path), "%s.csv", prefix);
    if (*prefix == '\0' || raw_length < 0 || csv_length < 0 ||
        static_cast<std::size_t>(raw_length) >= sizeof(raw_path) ||
        static_cast<std::size_t>(csv_length) >= sizeof(csv_path) ||
        raw_capacity > kRawCapacity || record_capacity > kRecordCapacity ||
        raw_capacity == 0U || record_capacity == 0U) {
        std::fprintf(stderr, "receive diagnostic: invalid output prefix or capacity\n");
        return false;
    }
    // Allocate and pre-touch before stream submission. No allocation,
    // formatting, or output-file I/O occurs in the capture path.
    raw_.reset(new (std::nothrow) std::uint8_t[raw_capacity]{});
    records_.reset(new (std::nothrow) Record[record_capacity]{});
    if (!raw_ || !records_) {
        raw_.reset();
        records_.reset();
        std::fprintf(stderr, "receive diagnostic: bounded buffer allocation failed\n");
        return false;
    }
    touch_buffer(raw_.get(), raw_capacity);
    touch_buffer(records_.get(), record_capacity * sizeof(Record));
    raw_file_ = open_receive_diagnostic_file(raw_path);
    if (raw_file_ == nullptr) {
        std::fprintf(stderr, "receive diagnostic: cannot exclusively create %s: %s\n",
                     raw_path, std::strerror(errno));
    } else {
        csv_file_ = open_receive_diagnostic_file(csv_path);
        if (csv_file_ == nullptr) {
            std::fprintf(stderr,
                         "receive diagnostic: cannot exclusively create %s: %s; "
                         "new empty file %s remains\n",
                         csv_path, std::strerror(errno), raw_path);
        }
    }
    if (raw_file_ == nullptr || csv_file_ == nullptr) {
        (void)close_files();
        raw_.reset();
        records_.reset();
        return false;
    }
    raw_capacity_ = raw_capacity;
    record_capacity_ = record_capacity;
    raw_size_ = record_count_ = 0U;
    submissions_ = completions_ = observed_valid_bytes_ = invalid_lengths_ = 0U;
    decoder_bytes_ = dropped_records_ = dropped_raw_bytes_ = 0U;
    draining_ = false;
    epoch_ = std::chrono::steady_clock::now();
    enabled_ = true;
    std::fprintf(stderr,
                 "receive diagnostic: reserved %s and %s; bounded in-memory capture "
                 "adds timing/copy overhead; files written only after callback drain\n",
                 raw_path, csv_path);
    return true;
}

std::uint64_t ReceiveDiagnostic::stamp() const noexcept
{
    // One-based nanoseconds distinguish a measured instant from an absent stage.
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - epoch_).count()) + 1U;
}

ReceiveDiagnostic::Record* ReceiveDiagnostic::begin_submission(
    std::uint32_t slot, std::uint64_t generation, std::uint64_t order,
    std::uint32_t requested) noexcept
{
    if (!enabled_) {
        return nullptr;
    }
    ++submissions_;
    if (record_count_ == record_capacity_) {
        ++dropped_records_;
        return nullptr;
    }
    Record& record = records_[record_count_++];
    record.slot = slot;
    record.generation = generation;
    record.submission_order = order;
    record.requested_bytes = requested;
    record.submit_begin_ns = stamp();
    return &record;
}

void ReceiveDiagnostic::submitted(Record* record, int result) noexcept
{
    if (record != nullptr) {
        record->submit_end_ns = stamp();
        record->submit_result = result;
        if (result != 0) {
            record->disposition = Disposition::submit_failed;
        }
    }
}

void ReceiveDiagnostic::completed(Record* record, int status, int actual,
                                  bool valid_length, std::uint64_t entry_ns) noexcept
{
    if (!enabled_) {
        return;
    }
    ++completions_;
    if (valid_length) {
        observed_valid_bytes_ += static_cast<std::uint64_t>(actual);
    } else {
        ++invalid_lengths_;
    }
    if (record != nullptr) {
        record->completion_order = completions_;
        record->callback_entry_ns = entry_ns;
        record->status = status;
        record->actual_bytes = actual;
        record->callback_during_drain = draining_;
        record->disposition = valid_length ? Disposition::not_decoded
                                           : Disposition::invalid_length;
        record->callback_ready_ns = stamp();
    }
}

void ReceiveDiagnostic::decoder_input(Record* record, const std::uint8_t* bytes,
                                      std::size_t size) noexcept
{
    if (!enabled_) {
        return;
    }
    // Once metadata is full, no orphan raw bytes are appended. Saved data is
    // always a prefix of decoder input, with omissions explicit in the footer.
    const std::size_t count = record == nullptr ? 0U : std::min(size, raw_capacity_ - raw_size_);
    if (record != nullptr) {
        record->input_offset = decoder_bytes_;
        record->raw_offset = raw_size_;
        record->raw_bytes = count;
        record->raw_dropped_bytes = size - count;
        record->disposition = Disposition::decoded;
    }
    if (count != 0U) {
        std::memcpy(raw_.get() + raw_size_, bytes, count);
    }
    raw_size_ += count;
    decoder_bytes_ += size;
    dropped_raw_bytes_ += size - count;
    if (record != nullptr) {
        record->raw_copy_end_ns = stamp();
    }
}

// Keep the header and values generated from the same field list. All values are
// numeric; comments carry schema and finalization metadata, never device keys.
#define RX_FIELDS(U, I, B, D) \
    U(submission_order) U(generation) U(completion_order) \
    U(submit_begin_ns) U(submit_end_ns) U(callback_entry_ns) U(callback_ready_ns) \
    U(processing_begin_ns) U(raw_copy_end_ns) U(decode_begin_ns) U(decode_end_ns) \
    U(processing_end_ns) U(emit_first_ns) U(emit_last_ns) U(emit_total_ns) \
    U(emit_calls) U(emit_accepted) U(input_offset) U(raw_offset) U(raw_bytes) \
    U(raw_dropped_bytes) U(sync_loss_delta) U(sync_discard_delta) \
    U(framer_accepted_delta) U(framer_emitted_delta) U(framer_pending_after) \
    U(slot) U(requested_bytes) I(submit_result) I(status) I(actual_bytes) \
    I(decode_error) D(disposition) B(callback_during_drain)

bool ReceiveDiagnostic::flush_after_drain(unsigned terminal_result) noexcept
{
    if (!enabled_) {
        return true;
    }
    bool ok = std::fwrite(raw_.get(), 1U, raw_size_, raw_file_) == raw_size_;
    ok = std::fclose(raw_file_) == 0 && ok;
    raw_file_ = nullptr;
    ok = std::fprintf(csv_file_,
                      "# asicen_rx_diagnostic=1\n"
                      "# timestamps=host_steady_clock_nanoseconds_since_init_plus_one\n"
                      "# callback_time=host_observation_not_usb_bus_arrival\n"
                      "# raw_capacity=%zu\n# record_capacity=%zu\n",
                      raw_capacity_, record_capacity_) >= 0 && ok;
    const char* separator = "";
#define RX_HEADER(field) \
    ok = std::fprintf(csv_file_, "%s%s", separator, #field) >= 0 && ok; separator = ",";
    RX_FIELDS(RX_HEADER, RX_HEADER, RX_HEADER, RX_HEADER)
#undef RX_HEADER
    ok = std::fputc('\n', csv_file_) != EOF && ok;
    for (std::size_t i = 0U; i < record_count_; ++i) {
        const Record& row = records_[i];
        separator = "";
#define RX_UNSIGNED(field) \
    ok = std::fprintf(csv_file_, "%s%" PRIu64, separator, \
                      static_cast<std::uint64_t>(row.field)) >= 0 && ok; separator = ",";
#define RX_SIGNED(field) \
    ok = std::fprintf(csv_file_, "%s%d", separator, row.field) >= 0 && ok; separator = ",";
        RX_FIELDS(RX_UNSIGNED, RX_SIGNED, RX_UNSIGNED, RX_UNSIGNED)
#undef RX_UNSIGNED
#undef RX_SIGNED
        ok = std::fputc('\n', csv_file_) != EOF && ok;
    }
    ok = std::fprintf(csv_file_,
                      "# stored_rows=%zu\n# dropped_records=%" PRIu64 "\n"
                      "# raw_bytes_saved=%zu\n# raw_bytes_omitted=%" PRIu64 "\n"
                      "# observed_valid_bytes=%" PRIu64 "\n# decoder_input_bytes=%" PRIu64 "\n"
                      "# submissions=%" PRIu64 "\n# completions=%" PRIu64 "\n"
                      "# invalid_lengths=%" PRIu64 "\n# terminal_result=%u\n",
                      record_count_, dropped_records_, raw_size_, dropped_raw_bytes_,
                      observed_valid_bytes_, decoder_bytes_, submissions_, completions_,
                      invalid_lengths_, terminal_result) >= 0 && ok;
    // A missing final marker means partial/untrustworthy output. A killed
    // process or fatal callback drain can leave the initially reserved files empty.
    ok = std::fflush(csv_file_) == 0 && ok;
    if (ok) {
        ok = std::fprintf(csv_file_, "# finalized=1\n") >= 0;
    }
    ok = std::fclose(csv_file_) == 0 && ok;
    csv_file_ = nullptr;
    if (ok) {
        std::fprintf(stderr,
                     "receive diagnostic: saved %zu raw bytes and %zu rows; "
                     "omitted %" PRIu64 " decoder bytes and %" PRIu64 " rows\n",
                     raw_size_, record_count_, dropped_raw_bytes_, dropped_records_);
    } else {
        std::fprintf(stderr, "receive diagnostic: output write/close failed; files may be partial\n");
    }
    enabled_ = false;
    raw_.reset();
    records_.reset();
    return ok;
}
#undef RX_FIELDS

}  // namespace asicen
