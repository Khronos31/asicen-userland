// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_PRODUCT_PROFILE_H
#define ASICEN_USERLAND_PRODUCT_PROFILE_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace asicen::profile {

constexpr bool valid_instance(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 80U || value == "." || value == "..") return false;

    bool numeric = true;
    for (const unsigned char character : value) {
        const bool digit = character >= '0' && character <= '9';
        const bool accepted = digit || (character >= 'a' && character <= 'z') ||
                              (character >= 'A' && character <= 'Z') ||
                              character == '_' || character == '-' || character == '.';
        if (!accepted) return false;
        numeric = numeric && digit;
    }
    return !numeric || (value.size() != 14U && value.size() != 15U);
}

// Maximum/default W3U3 topology; runtime enclosures may contain 1 or 2 receivers.
inline constexpr std::uint8_t kReceiverCount = 4U;
inline constexpr std::uint8_t kUsbFunctionCount = 2U;
inline constexpr std::uint8_t kReceiversPerUsbFunction = 2U;
inline constexpr std::uint8_t kUsbPresentMask = 0x03U;
inline constexpr const char* kRuntimeDirectoryName = "asicen-userland";
inline constexpr const char* kIpcMagic = "ASCN";

constexpr bool valid_receiver_count(std::uint8_t count) noexcept
{
    return count == 1U || count == 2U || count == 4U;
}

constexpr std::uint8_t usb_function_count(std::uint8_t count) noexcept
{
    return valid_receiver_count(count) ? (count == 4U ? 2U : 1U) : 0U;
}

constexpr std::uint8_t usb_present_mask(std::uint8_t count) noexcept
{
    return valid_receiver_count(count) ? (count == 4U ? 0x03U : 0x01U) : 0U;
}

constexpr bool is_combined_receiver(std::uint8_t count, std::uint8_t receiver) noexcept
{
    return count == 1U && receiver == 0U;
}

constexpr std::uint8_t usb_function_for_receiver(std::uint8_t receiver) noexcept
{
    return static_cast<std::uint8_t>(receiver / kReceiversPerUsbFunction);
}

constexpr std::uint8_t local_receiver_for(std::uint8_t receiver) noexcept
{
    return static_cast<std::uint8_t>(receiver % kReceiversPerUsbFunction);
}

constexpr std::uint8_t receiver_for(std::uint8_t usb_function, std::uint8_t local_receiver) noexcept
{
    return static_cast<std::uint8_t>(usb_function * kReceiversPerUsbFunction + local_receiver);
}

constexpr bool is_satellite_receiver(std::uint8_t receiver) noexcept
{
    return local_receiver_for(receiver) == 0U;
}

static_assert(receiver_for(0, 0) == 0 && receiver_for(0, 1) == 1 && receiver_for(1, 0) == 2 &&
              receiver_for(1, 1) == 3);

}  // namespace asicen::profile

#endif  // ASICEN_USERLAND_PRODUCT_PROFILE_H
