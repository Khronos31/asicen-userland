#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>

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
};

bool parse_cf40_read_response(int transferred, const unsigned char* response,
                              std::uint8_t* value);
bool cf40_write_response_complete(int transferred);

class CaptureBackend {
public:
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
