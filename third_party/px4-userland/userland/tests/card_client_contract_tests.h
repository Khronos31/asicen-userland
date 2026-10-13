// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_TEST_CARD_CLIENT_CONTRACT_H
#define PX4_USERLAND_TEST_CARD_CLIENT_CONTRACT_H

#include "px4/control_client.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace px4::userland::test {

namespace card_client_contract {

template <typename Payload>
inline Result<ipc::posix::ControlResponse> request(
    ipc::posix::PosixControlClient& client, ipc::MessageType type,
    const Payload& payload) noexcept
{
    std::array<std::uint8_t, ipc::kMaxControlPayload> encoded{};
    const auto size = ipc::encode_payload(
        payload, MutableByteView{encoded.data(), encoded.size()});
    if (!size) return Result<ipc::posix::ControlResponse>::failure(size.error());
    return client.request(type, ByteView{encoded.data(), size.value()}, Timeout{2000U});
}

inline Result<std::uint64_t> connect_card(ipc::posix::PosixControlClient& client,
                                        ipc::ShareMode mode) noexcept
{
    const auto response = request(client, ipc::MessageType::CARD_CONNECT,
                                 ipc::CardConnectRequestPayload{mode});
    if (!response) return Result<std::uint64_t>::failure(response.error());
    const auto card = ipc::decode_card_connect_response_payload(
        ByteView{response.value().payload.data(), response.value().payload.size()});
    if (!card) return Result<std::uint64_t>::failure(card.error());
    return Result<std::uint64_t>::success(card.value().card_handle);
}

}  // namespace card_client_contract

// Exercise the existing IPC v1 ownership contract through real client/server
// framing. The caller supplies a running mock server with a present card.
// Share this exact scenario between POSIX and native Windows socket tests;
// it does not implement or imply a WinSCard API contract.
inline bool run_card_client_contract(const ipc::posix::EndpointConfig& endpoint)
{
    using namespace ipc;
    using namespace ipc::posix;
    using card_client_contract::connect_card;
    using card_client_contract::request;

#define PX4_CARD_CLIENT_CHECK(condition)                                                  \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            std::fprintf(stderr, "card client contract failed at %s:%d: %s\n",          \
                         __FILE__, __LINE__, #condition);                                \
            return false;                                                                \
        }                                                                                \
    } while (false)

    auto first = PosixControlClient::connect(endpoint, kCapabilityCard, Timeout{2000U});
    auto second = PosixControlClient::connect(endpoint, kCapabilityCard, Timeout{2000U});
    auto third = PosixControlClient::connect(endpoint, kCapabilityCard, Timeout{2000U});
    PX4_CARD_CLIENT_CHECK(first && second && third);
    const auto first_card = connect_card(*first.value(), ShareMode::shared);
    const auto second_card = connect_card(*second.value(), ShareMode::shared);
    PX4_CARD_CLIENT_CHECK(first_card && second_card && first_card.value() != 0U &&
                          second_card.value() != 0U &&
                          first_card.value() != second_card.value());
    const auto denied_exclusive = connect_card(*third.value(), ShareMode::exclusive);
    PX4_CARD_CLIENT_CHECK(!denied_exclusive && denied_exclusive.error() == Error::BUSY);

    // A valid numeric handle from another connection is still not owned.
    const auto foreign_reset = request(*second.value(), MessageType::CARD_RESET,
                                       CardHandleRequestPayload{first_card.value()});
    PX4_CARD_CLIENT_CHECK(!foreign_reset && foreign_reset.error() == Error::NOT_FOUND);
    const auto denied_upgrade = request(
        *second.value(), MessageType::CARD_RECONNECT,
        CardReconnectRequestPayload{second_card.value(), ShareMode::exclusive,
                                    Disposition::leave});
    PX4_CARD_CLIENT_CHECK(!denied_upgrade && denied_upgrade.error() == Error::BUSY);

    PX4_CARD_CLIENT_CHECK(request(*first.value(), MessageType::BEGIN_TRANSACTION,
                                  CardHandleRequestPayload{first_card.value()}));
    const auto competing_begin = request(*second.value(), MessageType::BEGIN_TRANSACTION,
                                          CardHandleRequestPayload{second_card.value()});
    PX4_CARD_CLIENT_CHECK(!competing_begin && competing_begin.error() == Error::BUSY);
    const std::array<std::uint8_t, 2U> apdu{0x00U, 0x84U};
    const auto transmit_second = [&]() noexcept {
        return request(*second.value(), MessageType::CARD_TRANSMIT,
                       CardTransmitRequestPayload{second_card.value(),
                                                  ByteView{apdu.data(), apdu.size()}});
    };
    const auto competing_transmit = transmit_second();
    PX4_CARD_CLIENT_CHECK(!competing_transmit && competing_transmit.error() == Error::BUSY);

    // Reset by the transaction owner must not unlock the competing handle.
    PX4_CARD_CLIENT_CHECK(request(*first.value(), MessageType::CARD_RESET,
                                  CardHandleRequestPayload{first_card.value()}));
    const auto still_busy = transmit_second();
    PX4_CARD_CLIENT_CHECK(!still_busy && still_busy.error() == Error::BUSY);
    PX4_CARD_CLIENT_CHECK(request(
        *first.value(), MessageType::END_TRANSACTION,
        CardDispositionRequestPayload{first_card.value(), Disposition::reset}));
    const auto after_end = transmit_second();
    PX4_CARD_CLIENT_CHECK(after_end);
    const auto response = decode_card_transmit_response_payload(
        ByteView{after_end.value().payload.data(), after_end.value().payload.size()});
    PX4_CARD_CLIENT_CHECK(response && response.value().response.size == 2U);

    // Transport teardown releases the transaction and handle even without
    // END_TRANSACTION or CARD_DISCONNECT. Wait for that observable outcome,
    // never inspect mutable CardService state from the client thread.
    PX4_CARD_CLIENT_CHECK(request(*first.value(), MessageType::BEGIN_TRANSACTION,
                                  CardHandleRequestPayload{first_card.value()}));
    first.value()->close();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        const auto recovered = request(*second.value(), MessageType::BEGIN_TRANSACTION,
                                        CardHandleRequestPayload{second_card.value()});
        if (recovered) break;
        PX4_CARD_CLIENT_CHECK(recovered.error() == Error::BUSY &&
                              std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    PX4_CARD_CLIENT_CHECK(request(
        *second.value(), MessageType::END_TRANSACTION,
        CardDispositionRequestPayload{second_card.value(), Disposition::leave}));

    const auto upgraded = request(
        *second.value(), MessageType::CARD_RECONNECT,
        CardReconnectRequestPayload{second_card.value(), ShareMode::exclusive,
                                    Disposition::reset});
    PX4_CARD_CLIENT_CHECK(upgraded);
    PX4_CARD_CLIENT_CHECK(decode_atr_payload(
        ByteView{upgraded.value().payload.data(), upgraded.value().payload.size()}));
    const auto denied_shared = connect_card(*third.value(), ShareMode::shared);
    PX4_CARD_CLIENT_CHECK(!denied_shared && denied_shared.error() == Error::BUSY);
    PX4_CARD_CLIENT_CHECK(request(
        *second.value(), MessageType::CARD_DISCONNECT,
        CardDispositionRequestPayload{second_card.value(), Disposition::reset}));
    const auto stale_transmit = transmit_second();
    PX4_CARD_CLIENT_CHECK(!stale_transmit && stale_transmit.error() == Error::NOT_FOUND);

    const auto final_card = connect_card(*third.value(), ShareMode::exclusive);
    PX4_CARD_CLIENT_CHECK(final_card);
    PX4_CARD_CLIENT_CHECK(request(
        *third.value(), MessageType::CARD_DISCONNECT,
        CardDispositionRequestPayload{final_card.value(), Disposition::leave}));
    return true;

#undef PX4_CARD_CLIENT_CHECK
}

// Verify HELLO policy reaches the serialized service across real framing and
// worker dispatch. Presence is the only shared mutable input to the mock.
inline bool run_card_generation_contract(const ipc::posix::EndpointConfig& endpoint,
                                         std::atomic<bool>& present)
{
    using namespace ipc;
    using namespace ipc::posix;
    using card_client_contract::connect_card;
    using card_client_contract::request;
#define PX4_GENERATION_CHECK(condition)                                                   \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            std::fprintf(stderr, "card generation contract failed at %s:%d: %s\n",      \
                         __FILE__, __LINE__, #condition);                                \
            present.store(true);                                                         \
            return false;                                                                \
        }                                                                                \
    } while (false)
    constexpr auto strict_capabilities = kCapabilityCard | kCapabilityCardGenerationBinding;
    auto strict = PosixControlClient::connect(endpoint, strict_capabilities, Timeout{2000U});
    auto legacy = PosixControlClient::connect(endpoint, kCapabilityCard, Timeout{2000U});
    PX4_GENERATION_CHECK(strict && legacy);
    PX4_GENERATION_CHECK(strict.value()->negotiated_capabilities() == strict_capabilities);
    PX4_GENERATION_CHECK(legacy.value()->negotiated_capabilities() == kCapabilityCard);
    const auto first = connect_card(*strict.value(), ShareMode::shared);
    const auto second = connect_card(*strict.value(), ShareMode::shared);
    const auto old = connect_card(*legacy.value(), ShareMode::shared);
    PX4_GENERATION_CHECK(first && second && old);
    const auto handle_status = request(*strict.value(), MessageType::CARD_HANDLE_STATUS,
                                       CardHandleRequestPayload{first.value()});
    PX4_GENERATION_CHECK(handle_status);
    const auto initial_snapshot = decode_card_status_response_payload(
        ByteView{handle_status.value().payload.data(), handle_status.value().payload.size()});
    PX4_GENERATION_CHECK(initial_snapshot && initial_snapshot.value().present == 1U &&
                        initial_snapshot.value().initialized == 1U && initial_snapshot.value().atr.size > 0U);
    const auto foreign_status = request(*strict.value(), MessageType::CARD_HANDLE_STATUS,
                                        CardHandleRequestPayload{old.value()});
    PX4_GENERATION_CHECK(!foreign_status && foreign_status.error() == Error::NOT_FOUND);
    const auto observe = [&](bool inserted) {
        present.store(inserted);
        const auto status = legacy.value()->request(MessageType::CARD_STATUS, ByteView{},
                                                     Timeout{2000U});
        if (!status) return false;
        const auto decoded = decode_card_status_response_payload(
            ByteView{status.value().payload.data(), status.value().payload.size()});
        return decoded && (decoded.value().present != 0U) == inserted;
    };
    const std::array<std::uint8_t, 2U> apdu{0x00U, 0x84U};
    const auto send = [&](PosixControlClient& client, std::uint64_t handle) {
        return request(client, MessageType::CARD_TRANSMIT,
                       CardTransmitRequestPayload{handle, ByteView{apdu.data(), apdu.size()}});
    };
    PX4_GENERATION_CHECK(request(*strict.value(), MessageType::BEGIN_TRANSACTION,
                                CardHandleRequestPayload{first.value()}));
    PX4_GENERATION_CHECK(observe(false));
    PX4_GENERATION_CHECK(observe(true));
    const auto stale = send(*strict.value(), first.value());
    PX4_GENERATION_CHECK(!stale && stale.error() == Error::CARD_REMOVED);
    const auto stale_status = request(*strict.value(), MessageType::CARD_HANDLE_STATUS,
                                      CardHandleRequestPayload{first.value()});
    PX4_GENERATION_CHECK(!stale_status && stale_status.error() == Error::CARD_REMOVED);
    const auto transaction = request(*strict.value(), MessageType::BEGIN_TRANSACTION,
                                      CardHandleRequestPayload{second.value()});
    PX4_GENERATION_CHECK(!transaction && transaction.error() == Error::CARD_REMOVED);
    PX4_GENERATION_CHECK(send(*legacy.value(), old.value()));
    const auto still_stale = send(*strict.value(), first.value());
    PX4_GENERATION_CHECK(!still_stale && still_stale.error() == Error::CARD_REMOVED);
    PX4_GENERATION_CHECK(request(
        *strict.value(), MessageType::CARD_RECONNECT,
        CardReconnectRequestPayload{first.value(), ShareMode::shared, Disposition::leave}));
    PX4_GENERATION_CHECK(send(*strict.value(), first.value()));
    PX4_GENERATION_CHECK(request(*strict.value(), MessageType::CARD_HANDLE_STATUS,
                                CardHandleRequestPayload{first.value()}));
    const auto other_stale = send(*strict.value(), second.value());
    PX4_GENERATION_CHECK(!other_stale && other_stale.error() == Error::CARD_REMOVED);
    PX4_GENERATION_CHECK(request(*strict.value(), MessageType::CARD_RESET,
                                CardHandleRequestPayload{second.value()}));
    PX4_GENERATION_CHECK(send(*strict.value(), second.value()));
    PX4_GENERATION_CHECK(observe(false));
    PX4_GENERATION_CHECK(observe(true));
    PX4_GENERATION_CHECK(request(*strict.value(), MessageType::CARD_DISCONNECT,
                                CardDispositionRequestPayload{first.value(), Disposition::reset}));
    PX4_GENERATION_CHECK(request(*strict.value(), MessageType::CARD_DISCONNECT,
                                CardDispositionRequestPayload{second.value(), Disposition::leave}));
    PX4_GENERATION_CHECK(request(*legacy.value(), MessageType::CARD_DISCONNECT,
                                CardDispositionRequestPayload{old.value(), Disposition::leave}));
    return true;
#undef PX4_GENERATION_CHECK
}

}  // namespace px4::userland::test

#endif  // PX4_USERLAND_TEST_CARD_CLIENT_CONTRACT_H
