#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace asicen {

enum class CardResult : std::uint8_t {
    Ok,
    NoCard,
    Busy,
    ProtocolError,
    IoError,
    Unsupported,
};

class CardBackend {
public:
    virtual ~CardBackend() = default;

    virtual CardResult initialize() = 0;
    virtual CardResult card_present(bool* present) = 0;
    virtual CardResult reset(std::vector<std::uint8_t>* atr) = 0;
    virtual CardResult transmit(const std::uint8_t* command,
                                std::size_t command_size,
                                std::vector<std::uint8_t>* response) = 0;
};

}  // namespace asicen
