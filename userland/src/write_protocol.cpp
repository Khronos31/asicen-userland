#include "asicen/write_protocol.h"

#include <algorithm>
#include <limits>

namespace asicen {
namespace {

std::uint16_t pack_two(const std::uint8_t* data, std::size_t size)
{
    std::uint16_t word = 0;
    if (size > 0) {
        word = data[0];
    }
    if (size > 1) {
        word |= static_cast<std::uint16_t>(data[1]) << 8U;
    }
    return word;
}

} // namespace

bool make_i2c_write_chunk(std::uint8_t slave, std::uint8_t reg, const std::uint8_t* data,
                          std::size_t size, bool no_stop, ControlTransfer* out,
                          std::uint16_t timeout_ms)
{
    if (out == nullptr || data == nullptr || size == 0 || size > 2) {
        return false;
    }

    *out = ControlTransfer{
        0,
        no_stop ? Request::I2cWriteNoStop : Request::I2cWrite,
        setup_word(slave, reg),
        pack_two(data, size),
        static_cast<std::uint16_t>(size + 1U),
        Direction::In,
        timeout_ms,
    };
    return true;
}

bool make_i2c_buffer_fill(std::uint8_t staging_offset, const std::uint8_t* data, std::size_t size,
                          ControlTransfer* out, std::uint16_t timeout_ms)
{
    if (out == nullptr || data == nullptr || size == 0 || size > 3) {
        return false;
    }

    const std::uint8_t d0 = data[0];
    const std::uint8_t d1 = size > 1 ? data[1] : 0;
    const std::uint8_t d2 = size > 2 ? data[2] : 0;
    *out = ControlTransfer{
        0,
        Request::I2cBufferFill,
        setup_word(staging_offset, d0),
        setup_word(d1, d2),
        static_cast<std::uint16_t>(size + 1U),
        Direction::In,
        timeout_ms,
    };
    return true;
}

ControlTransfer make_i2c_buffer_send(std::uint8_t slave, std::uint16_t staged_length, bool stop,
                                     std::uint16_t timeout_ms)
{
    if (staged_length == 0U || staged_length > 0x20U) {
        ControlTransfer invalid{};
        invalid.valid = false;
        return invalid;
    }
    return ControlTransfer{
        0,
        Request::I2cBufferSend,
        setup_word(slave, stop ? 1U : 0U),
        0,
        static_cast<std::uint16_t>(staged_length + 1U),
        Direction::In,
        timeout_ms,
    };
}

bool make_sysctrl_write(std::uint8_t address, const std::uint8_t* data, std::size_t size,
                        ControlTransfer* out, std::uint16_t timeout_ms)
{
    if (out == nullptr || data == nullptr || size == 0 || size > 3) {
        return false;
    }

    const std::uint8_t d0 = data[0];
    const std::uint8_t d1 = size > 1 ? data[1] : 0;
    const std::uint8_t d2 = size > 2 ? data[2] : 0;
    *out = ControlTransfer{
        0,
        Request::SysCtrlWrite,
        setup_word(address, d0),
        setup_word(d1, d2),
        static_cast<std::uint16_t>(size + 1U),
        Direction::In,
        timeout_ms,
    };
    return true;
}

ControlTransfer make_gpio_set(std::uint8_t value, std::uint8_t mask, std::uint16_t timeout_ms)
{
    return ControlTransfer{0,         Request::Gpio, setup_word(value, mask), 0, 1, Direction::In,
                           timeout_ms};
}

ControlTransfer make_gpio_ex_set(std::uint8_t value, std::uint8_t mask, std::uint16_t timeout_ms)
{
    return ControlTransfer{
        0, Request::GpioExSet, setup_word(value, mask), 0, 1, Direction::In, timeout_ms};
}

ControlTransfer make_gpio_ex_get(std::uint16_t timeout_ms)
{
    return ControlTransfer{0, Request::GpioExGet, 0, 0, 1, Direction::In, timeout_ms};
}

ControlTransfer make_dsc_control(std::uint8_t tuner_num, bool stop, std::uint16_t timeout_ms)
{
    if (tuner_num >= kStreamLaneCount) {
        ControlTransfer invalid{};
        invalid.valid = false;
        return invalid;
    }
    return ControlTransfer{
        0, stop ? Request::DscStop : Request::DscStart, tuner_num, 0, 1, Direction::In, timeout_ms,
    };
}

ControlTransfer make_reset_channel(std::uint8_t first, std::uint8_t second,
                                   std::uint16_t timeout_ms)
{
    return ControlTransfer{
        0, Request::ResetChannel, setup_word(first, second), 0, 1, Direction::In, timeout_ms,
    };
}

ControlTransfer make_cf_read(std::uint8_t local, std::uint8_t subcmd, std::uint16_t length,
                             std::uint16_t timeout_ms)
{
    if (local >= kStreamLaneCount || length == std::numeric_limits<std::uint16_t>::max()) {
        ControlTransfer invalid{};
        invalid.valid = false;
        return invalid;
    }
    const std::uint8_t effective =
        static_cast<std::uint8_t>(local == 1 ? (subcmd | 0x80U) : subcmd);
    return ControlTransfer{
        0,
        Request::ChannelFilterRead,
        effective,
        0,
        static_cast<std::uint16_t>(length + 1U),
        Direction::In,
        timeout_ms,
    };
}

bool make_cf_write(std::uint8_t local, std::uint8_t subcmd, const std::uint8_t* data,
                   std::size_t size, ControlTransfer* out, std::uint16_t timeout_ms)
{
    if (local >= kStreamLaneCount || out == nullptr || data == nullptr || size == 0U || size > 3U) {
        return false;
    }
    const std::uint8_t effective =
        static_cast<std::uint8_t>(local == 1 ? (subcmd | 0x80U) : subcmd);
    const std::uint8_t d1 = size > 1 ? data[1] : 0;
    const std::uint8_t d2 = size > 2 ? data[2] : 0;
    *out = ControlTransfer{
        0,
        Request::ChannelFilterWrite,
        setup_word(effective, data[0]),
        setup_word(d1, d2),
        static_cast<std::uint16_t>(size + 1U),
        Direction::In,
        timeout_ms,
    };
    return true;
}

bool cf_chunk_write_response_complete(int transferred, std::size_t payload_size)
{
    return payload_size != 0 && payload_size <= 3 &&
           transferred == static_cast<int>(payload_size + 1U);
}

std::vector<ControlTransfer> build_cf_block_write_plan(std::uint8_t local, const std::uint8_t* data,
                                                       std::size_t size, std::uint16_t timeout_ms)
{
    std::vector<ControlTransfer> plan;
    if (local >= kStreamLaneCount || data == nullptr || size != 0x45U) {
        return plan;
    }
    for (std::size_t offset = 0; offset < size;) {
        const std::size_t chunk = std::min<std::size_t>(3, size - offset);
        ControlTransfer transfer{};
        if (!make_cf_write(local, static_cast<std::uint8_t>(offset), data + offset, chunk,
                           &transfer, timeout_ms)) {
            return {};
        }
        plan.push_back(transfer);
        offset += chunk;
    }
    return plan;
}

std::vector<ControlTransfer> build_i2c_write_sequence(std::uint8_t slave, std::uint8_t reg,
                                                      const std::uint8_t* data, std::size_t size,
                                                      std::uint8_t mode, std::uint16_t timeout_ms)
{
    std::vector<ControlTransfer> result;
    if (data == nullptr || size == 0 || size > 0xffffU) {
        return result;
    }

    if (mode == 2 || mode == 3) {
        std::size_t consumed = 0;
        while (consumed < size) {
            const std::size_t batch = std::min<std::size_t>(0x20, size - consumed);
            std::size_t staged = 0;
            while (staged < batch) {
                const std::size_t chunk = std::min<std::size_t>(3, batch - staged);
                ControlTransfer fill{};
                if (!make_i2c_buffer_fill(static_cast<std::uint8_t>(staged),
                                          data + consumed + staged, chunk, &fill, timeout_ms)) {
                    return {};
                }
                result.push_back(fill);
                staged += chunk;
            }
            result.push_back(make_i2c_buffer_send(slave, static_cast<std::uint16_t>(batch),
                                                  mode != 2, timeout_ms));
            consumed += batch;
        }
        return result;
    }

    std::size_t consumed = 0;
    std::uint8_t current_reg = reg;
    const bool no_stop = (mode & 0x04U) != 0;
    while (consumed < size) {
        // The historical code writes two bytes from an even register and one
        // from an odd register, then advances the register by the chunk size.
        std::size_t chunk = (current_reg & 1U) != 0 ? 1U : 2U;
        chunk = std::min(chunk, size - consumed);

        ControlTransfer transfer{};
        if (!make_i2c_write_chunk(slave, current_reg, data + consumed, chunk, no_stop, &transfer,
                                  timeout_ms)) {
            return {};
        }
        result.push_back(transfer);
        consumed += chunk;
        current_reg = static_cast<std::uint8_t>(current_reg + chunk);
    }
    return result;
}

} // namespace asicen
