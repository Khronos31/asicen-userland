#pragma once
// SPDX-License-Identifier: GPL-2.0-only
// Portable, RF-only ISDB-T path for the Newport Media chips used by V2.
#include <cstdint>

namespace asicen {
class V2NmiIo {
  public:
    virtual ~V2NmiIo() = default;
    virtual bool read(std::uint16_t reg, std::uint32_t *value, std::uint8_t width) = 0;
    virtual bool write(std::uint16_t reg, std::uint32_t value, std::uint8_t width) = 0;
    virtual bool demod_write(std::uint8_t reg, std::uint8_t value) = 0;
    virtual bool delay_ms(unsigned milliseconds) = 0;
    virtual bool healthy() const = 0;
};
enum class V2NmiResult : std::uint8_t {
    Completed,
    UnsupportedChip,
    InvalidArgument,
    Failed,
    CalibrationTimeout,
};
enum class V2NmiFamily : std::uint8_t {
    Unsupported,
    Nm120,
    Nm130,
    Nm131,
    Extended813000,
};
V2NmiFamily v2_nmi_family(std::uint32_t chip_id);
V2NmiResult initialize_v2_nmi(V2NmiIo &io, std::uint32_t *chip_id);
// hz is the already-offset tuner frequency, not the channel center in kHz.
// Uses vendor ISDB-T standard 6, output mode 2, 4 MHz digital IF input.
// No GPIO, firmware, link initialization, card or LNB operations are included.
V2NmiResult tune_v2_nmi(V2NmiIo &io, std::uint32_t hz, std::uint32_t chip_id);
} // namespace asicen
