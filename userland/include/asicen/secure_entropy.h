// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace asicen {

enum class EntropyReadStatus { Data, Interrupted, Error };

struct EntropyRead {
    EntropyReadStatus status = EntropyReadStatus::Error;
    std::size_t bytes = 0U;
};

class SecureEntropyIo {
public:
    virtual ~SecureEntropyIo() = default;
    virtual bool open() noexcept = 0;
    virtual EntropyRead read(std::uint8_t* output, std::size_t size) noexcept = 0;
    virtual bool close() noexcept = 0;
};

// Fill the complete output or clear every byte on failure. The caller owns
// the existing hardware preparation/rollback decision.
bool fill_secure_entropy_with_io(SecureEntropyIo& io, std::uint8_t* output,
                                 std::size_t size) noexcept;
bool fill_secure_entropy(std::uint8_t* output, std::size_t size) noexcept;

}  // namespace asicen
