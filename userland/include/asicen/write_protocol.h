#ifndef ASICEN_USERLAND_WRITE_PROTOCOL_H
#define ASICEN_USERLAND_WRITE_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "asicen/protocol.h"

namespace asicen {

// Historical W3U3 Linux userspace writes use vendor-IN control transfers.
// The write payload is packed into wValue/wIndex and the USB data stage
// returns a status byte (plus padding matching the encoded payload length).
bool make_i2c_write_chunk(std::uint8_t slave, std::uint8_t reg, const std::uint8_t* data,
                          std::size_t size, bool no_stop, ControlTransfer* out,
                          std::uint16_t timeout_ms = 1000);

bool make_i2c_buffer_fill(std::uint8_t staging_offset, const std::uint8_t* data, std::size_t size,
                          ControlTransfer* out, std::uint16_t timeout_ms = 1000);

ControlTransfer make_i2c_buffer_send(std::uint8_t slave, std::uint16_t staged_length, bool stop,
                                     std::uint16_t timeout_ms = 1000);

bool make_sysctrl_write(std::uint8_t address, const std::uint8_t* data, std::size_t size,
                        ControlTransfer* out, std::uint16_t timeout_ms = 1000);

ControlTransfer make_gpio_set(std::uint8_t value, std::uint8_t mask,
                              std::uint16_t timeout_ms = 1000);
ControlTransfer make_gpio_ex_set(std::uint8_t value, std::uint8_t mask,
                                 std::uint16_t timeout_ms = 1000);
ControlTransfer make_gpio_ex_get(std::uint16_t timeout_ms = 1000);
ControlTransfer make_dsc_control(std::uint8_t tuner_num, bool stop,
                                 std::uint16_t timeout_ms = 1000);
ControlTransfer make_reset_channel(std::uint8_t first, std::uint8_t second,
                                   std::uint16_t timeout_ms = 1000);

// Channel-filter (CF) transfers. Recovered from UsbDTV_u32ChannelFilter:
// read  uses bRequest 0x04, wValue = subcmd (|0x80 when local == 1), wIndex 0;
// write uses bRequest 0x05, wValue = subcmd | data[0]<<8,
//       wIndex = data[1] | data[2]<<8. Both return length+1 vendor-IN bytes.
ControlTransfer make_cf_read(std::uint8_t local, std::uint8_t subcmd, std::uint16_t length,
                             std::uint16_t timeout_ms = 1000);
bool make_cf_write(std::uint8_t local, std::uint8_t subcmd, const std::uint8_t* data,
                   std::size_t size, ControlTransfer* out, std::uint16_t timeout_ms = 1000);
bool cf_chunk_write_response_complete(int transferred, std::size_t payload_size);
std::vector<ControlTransfer> build_cf_block_write_plan(std::uint8_t local, const std::uint8_t* data,
                                                       std::size_t size,
                                                       std::uint16_t timeout_ms = 1000);

// Reconstructs the packetization performed by UsbDTV_u32I2C for writes.
// mode 2/3 uses the 0x0d staging buffer + 0x0e send path.
// other modes use 0x03, except modes with bit 2 set use 0x14 no-stop.
// Returns an empty vector for invalid input.
std::vector<ControlTransfer> build_i2c_write_sequence(std::uint8_t slave, std::uint8_t reg,
                                                      const std::uint8_t* data, std::size_t size,
                                                      std::uint8_t mode,
                                                      std::uint16_t timeout_ms = 1000);

} // namespace asicen

#endif // ASICEN_USERLAND_WRITE_PROTOCOL_H
