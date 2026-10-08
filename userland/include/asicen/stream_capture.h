#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <array>

namespace asicen {

// Raw finite stream capture seam. The loop is transport-agnostic so offline
// tests can inject partial timeouts, exact-limit USB errors, stop failures,
// zero data and slow/disconnected output without touching hardware.
enum class CaptureIo : std::uint8_t {
    Ok,
    Timeout,
    Error,
};

enum class CaptureOutcome : std::uint8_t {
    Completed,
    ZeroBytes,
    LimitNotReached,
    OutputFailed,
    UsbFailed,
    StartFailed,
    StopFailed,
    Cancelled,
    InvalidArgument,
};

struct CaptureRequest {
    std::uint8_t local = 1;
    std::uint8_t endpoint = 0;
    std::uint64_t byte_limit = 0;  // 0 = no packet-count limit
    std::chrono::steady_clock::time_point deadline{};
    std::size_t chunk_size = 4096;
};

struct CaptureStats {
    std::uint64_t bytes = 0;
    bool limit_reached = false;
    bool cf40_restore_failed = false;
    bool link_seed_cleanup_failed = false;
    bool link_seed_apply_failed = false;
    bool link_seed_state_unverifiable = false;
};

bool parse_cf40_read_response(int transferred, const unsigned char* response,
                              std::uint8_t* value);
bool cf40_write_response_complete(int transferred);

class CaptureBackend {
public:
    static constexpr std::size_t kCfBlockSize = 0x45;
    virtual ~CaptureBackend() = default;
    virtual bool dsc_start(std::uint8_t local) = 0;
    virtual bool dsc_stop(std::uint8_t local) = 0;
    virtual CaptureIo bulk_read(std::uint8_t endpoint, unsigned char* data, int length,
                                int* transferred, unsigned timeout_ms) = 0;
    // Explicitly opt-in CF40 diagnostic controls. Implementations should use
    // the recovered channel-filter protocol and require a complete transfer.
    virtual bool read_cf40(std::uint8_t local, std::uint8_t* value) {
        (void)local;
        (void)value;
        return false;
    }
    virtual bool write_cf40(std::uint8_t local, std::uint8_t value) {
        (void)local;
        (void)value;
        return false;
    }
    virtual bool read_cf_block(std::uint8_t local, std::uint8_t* data,
                               std::size_t size) {
        (void)local; (void)data; (void)size; return false;
    }
    virtual bool write_cf_block(std::uint8_t local, const std::uint8_t* data,
                                std::size_t size) {
        (void)local; (void)data; (void)size; return false;
    }
    virtual bool terrestrial_locked(std::uint8_t local, bool* locked,
                                    std::chrono::steady_clock::time_point deadline) {
        (void)local; (void)locked; (void)deadline; return false;
    }
    virtual bool filter_repeat_pulse(std::uint8_t local, std::uint8_t reset_state,
                                     std::chrono::steady_clock::time_point deadline) {
        (void)local; (void)reset_state; (void)deadline; return false;
    }
    // Explicit experimental write-only link diagnostic. The hardware seed
    // latch cannot be read back; implementations must never claim restoration.
    virtual bool snapshot_link_diagnostic() { return false; }
    virtual bool apply_link_seed() { return false; }
    virtual bool clear_link_seed_and_verify_controller() { return false; }
    virtual bool cancelled() const { return false; }
};

class CaptureOutput {
public:
    virtual ~CaptureOutput() = default;
    // Writes exactly size bytes or returns false (deadline, cancellation,
    // consumer disconnect or any write error).
    virtual bool write(const unsigned char* data, std::size_t size) = 0;
};

CaptureOutcome run_raw_capture(CaptureBackend* backend, CaptureOutput* output,
                               const CaptureRequest& request, CaptureStats* stats);

const char* capture_outcome_name(CaptureOutcome outcome);

// Keep capture command metadata on the diagnostic stream so stdout can carry
// only raw capture bytes. Other frontend commands retain their text summary.
void write_command_summary(std::ostream& normal, std::ostream& diagnostic,
                           bool capture_command, const char* model,
                           const char* port, std::uint8_t local);

}  // namespace asicen
