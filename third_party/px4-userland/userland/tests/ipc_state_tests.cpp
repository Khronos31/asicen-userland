// SPDX-License-Identifier: GPL-2.0-only
#include "px4/ipc.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

ByteView view(const std::vector<std::uint8_t>& value) noexcept
{
    return ByteView{value.data(), value.size()};
}

std::vector<std::uint8_t> make_frame(MessageType type, MessageKind kind,
                                     std::uint32_t request_id, ByteView payload)
{
    std::vector<std::uint8_t> frame(kFrameHeaderSize + payload.size, 0U);
    const FrameHeader header{kProtocolMajor, kProtocolMinor, type, kind, request_id,
                             static_cast<std::uint32_t>(payload.size)};
    const auto encoded = encode_frame(header, payload,
                                      MutableByteView{frame.data(), frame.size()});
    if (!encoded) {
        return {};
    }
    return frame;
}

template <typename T>
std::vector<std::uint8_t> make_typed_frame(MessageType type, MessageKind kind,
                                           std::uint32_t request_id, const T& payload)
{
    std::array<std::uint8_t, 512U> encoded_payload{};
    const auto encoded = encode_payload(
        payload, MutableByteView{encoded_payload.data(), encoded_payload.size()});
    if (!encoded) {
        return {};
    }
    return make_frame(type, kind, request_id,
                      ByteView{encoded_payload.data(), encoded.value()});
}

std::vector<std::uint8_t> make_empty_frame(MessageType type, MessageKind kind,
                                           std::uint32_t request_id)
{
    return make_frame(type, kind, request_id, ByteView{nullptr, 0U});
}

std::vector<std::uint8_t> make_error_frame(MessageType type, std::uint32_t request_id,
                                           ErrorCode error_code)
{
    const std::array<std::uint8_t, 5U> detail{'e', 'r', 'r', 'o', 'r'};
    return make_typed_frame(type, MessageKind::error_response, request_id,
                            ErrorResponsePayload{
                                error_code, ByteView{detail.data(), detail.size()}});
}

Result<ConnectionResult> process(ConnectionStateMachine& state,
                                 const std::vector<std::uint8_t>& wire,
                                 bool outbound)
{
    const auto frame = decode_frame(view(wire));
    if (!frame) {
        return Result<ConnectionResult>::failure(frame.error());
    }
    return outbound ? state.process_outbound(frame.value()) :
                      state.process_inbound(frame.value());
}

std::vector<std::uint8_t> hello_request(std::uint32_t id, std::uint32_t capabilities,
                                        std::uint16_t min_major = 1U,
                                        std::uint16_t min_minor = 0U,
                                        std::uint16_t max_major = 1U,
                                        std::uint16_t max_minor = 0U)
{
    return make_typed_frame(
        MessageType::HELLO, MessageKind::request, id,
        HelloRequestPayload{min_major, min_minor, max_major, max_minor, capabilities});
}

std::vector<std::uint8_t> hello_response(std::uint32_t id,
                                         std::uint32_t capabilities)
{
    return make_typed_frame(
        MessageType::HELLO, MessageKind::response, id,
        HelloResponsePayload{kProtocolMajor, kProtocolMinor, capabilities});
}

bool establish_control(ConnectionStateMachine& client, ConnectionStateMachine& server,
                       std::uint32_t requested, std::uint32_t supported,
                       std::uint32_t id = 1U)
{
    (void)supported;
    const auto request = hello_request(id, requested);
    const auto client_request = process(client, request, true);
    const auto server_request = process(server, request, false);
    if (!client_request || !server_request ||
        server_request.value().event != ConnectionEvent::hello_response_required) {
        return false;
    }
    const auto proposed = server.pending_hello_response();
    if (!proposed) {
        return false;
    }
    const auto response = hello_response(id, proposed.value().capabilities);
    return process(server, response, true) && process(client, response, false) &&
           client.phase() == ConnectionPhase::active &&
           server.phase() == ConnectionPhase::active;
}

std::vector<std::uint8_t> device_event()
{
    return make_typed_frame(
        MessageType::DEVICE_EVENT, MessageKind::event, 0U,
        DeviceEventPayload{7U, DeviceEventKind::state_changed,
                           EventTargetType::receiver, 3U});
}

bool test_control_happy_path_and_interleaved_event()
{
    constexpr std::uint32_t requested =
        kCapabilityEvents | kCapabilityCard | kCapabilityStreamStats | 0x80000000U;
    constexpr std::uint32_t supported = kCapabilityEvents | kCapabilityCard;
    ConnectionStateMachine client(ConnectionRole::control_client);
    ConnectionStateMachine server(ConnectionRole::control_server, supported);
    CHECK(establish_control(client, server, requested, supported, 0x101U));
    CHECK(client.negotiated_capabilities() == supported);
    CHECK(server.negotiated_capabilities() == supported);

    const auto request = make_typed_frame(
        MessageType::START_STREAM, MessageKind::request, 0x102U,
        LeaseRequestPayload{0x0102030405060708ULL});
    CHECK(process(client, request, true));
    CHECK(process(server, request, false));
    CHECK(client.has_outstanding_request() && server.has_outstanding_request());

    const auto event = device_event();
    CHECK(process(server, event, true));
    const auto received_event = process(client, event, false);
    CHECK(received_event && received_event.value().event == ConnectionEvent::event_accepted);
    CHECK(client.has_outstanding_request());

    const auto response = make_empty_frame(MessageType::START_STREAM,
                                           MessageKind::response, 0x102U);
    CHECK(process(server, response, true));
    CHECK(process(client, response, false));
    CHECK(!client.has_outstanding_request() && !server.has_outstanding_request());
    return true;
}

bool test_hello_negotiation_and_unknown_capabilities()
{
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        ConnectionStateMachine server(ConnectionRole::control_server,
                                      kKnownCapabilities | 0x40000000U);
        CHECK(establish_control(client, server,
                                kCapabilityEvents | kCapabilityStreamStats | 0x80000000U,
                                kCapabilityEvents | kCapabilityStreamStats, 0x201U));
        CHECK(server.negotiated_capabilities() ==
              (kCapabilityEvents | kCapabilityStreamStats));
    }
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        const auto request = hello_request(0x202U, kCapabilityEvents | 0x80000000U);
        CHECK(process(client, request, true));
        const auto response = hello_response(0x202U, kCapabilityEvents | 0x80000000U);
        CHECK(process(client, response, false));
        CHECK(client.negotiated_capabilities() == kCapabilityEvents);
    }
    {
        ConnectionStateMachine server(ConnectionRole::control_server, kKnownCapabilities);
        const auto request = hello_request(0x203U, 0U, 0U, 9U, 2U, 0U);
        const auto result = process(server, request, false);
        CHECK(result && result.value().event == ConnectionEvent::hello_response_required);
    }
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        ConnectionStateMachine server(ConnectionRole::control_server, kKnownCapabilities);
        const auto request = hello_request(0x204U, 0U, 2U, 0U, 2U, 1U);
        CHECK(process(client, request, true));
        const auto received = process(server, request, false);
        CHECK(received && received.value().event ==
                              ConnectionEvent::version_mismatch_response_required);
        CHECK(server.pending_hello_response().error() == Error::NOT_READY);
        const auto error = make_error_frame(MessageType::HELLO, 0x204U,
                                            ErrorCode::VERSION_MISMATCH);
        CHECK(process(server, error, true));
        CHECK(process(client, error, false));
        CHECK(server.closed() && client.closed());
    }
    {
        ConnectionStateMachine server(ConnectionRole::control_server, kKnownCapabilities);
        const auto request = hello_request(0x205U, 0U, 1U, 1U, 2U, 0U);
        const auto received = process(server, request, false);
        CHECK(received && received.value().event ==
                              ConnectionEvent::version_mismatch_response_required);
        const auto wrong = make_error_frame(MessageType::HELLO, 0x205U,
                                            ErrorCode::NOT_READY);
        CHECK(!process(server, wrong, true));
        CHECK(server.poisoned());
    }
    return true;
}

bool test_wrong_order_outstanding_and_correlation()
{
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        const auto wrong = make_empty_frame(MessageType::LIST, MessageKind::request, 1U);
        CHECK(!process(client, wrong, true) && client.poisoned());
    }
    {
        ConnectionStateMachine server(ConnectionRole::control_server, 0U);
        const auto wrong = make_empty_frame(MessageType::LIST, MessageKind::request, 1U);
        CHECK(!process(server, wrong, false) && server.poisoned());
        CHECK(process(server, hello_request(2U, 0U), false).error() ==
              Error::PROTOCOL_ERROR);
        server.reset();
        CHECK(server.phase() == ConnectionPhase::initial && !server.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        CHECK(!process(client, device_event(), false));
        CHECK(client.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        ConnectionStateMachine server(ConnectionRole::control_server, 0U);
        CHECK(establish_control(client, server, 0U, 0U));
        const auto first = make_typed_frame(MessageType::START_STREAM,
                                            MessageKind::request, 10U,
                                            LeaseRequestPayload{1U});
        const auto second = make_typed_frame(MessageType::RELEASE,
                                             MessageKind::request, 11U,
                                             LeaseRequestPayload{1U});
        CHECK(process(client, first, true));
        CHECK(!process(client, second, true) && client.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        ConnectionStateMachine server(ConnectionRole::control_server, 0U);
        CHECK(establish_control(client, server, 0U, 0U));
        const auto first = make_typed_frame(MessageType::START_STREAM,
                                            MessageKind::request, 20U,
                                            LeaseRequestPayload{1U});
        const auto second = make_typed_frame(MessageType::RELEASE,
                                             MessageKind::request, 21U,
                                             LeaseRequestPayload{1U});
        CHECK(process(server, first, false));
        CHECK(!process(server, second, false) && server.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        ConnectionStateMachine server(ConnectionRole::control_server, 0U);
        CHECK(establish_control(client, server, 0U, 0U));
        const auto request = make_typed_frame(MessageType::START_STREAM,
                                              MessageKind::request, 30U,
                                              LeaseRequestPayload{1U});
        CHECK(process(client, request, true));
        const auto wrong_type = make_empty_frame(MessageType::RELEASE,
                                                 MessageKind::response, 30U);
        CHECK(!process(client, wrong_type, false) && client.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        ConnectionStateMachine server(ConnectionRole::control_server, 0U);
        CHECK(establish_control(client, server, 0U, 0U));
        const auto request = make_typed_frame(MessageType::START_STREAM,
                                              MessageKind::request, 40U,
                                              LeaseRequestPayload{1U});
        CHECK(process(client, request, true));
        const auto wrong_id = make_empty_frame(MessageType::START_STREAM,
                                               MessageKind::response, 41U);
        CHECK(!process(client, wrong_id, false) && client.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        ConnectionStateMachine server(ConnectionRole::control_server, 0U);
        CHECK(establish_control(client, server, 0U, 0U));
        const auto request = make_typed_frame(MessageType::START_STREAM,
                                              MessageKind::request, 45U,
                                              LeaseRequestPayload{1U});
        CHECK(process(server, request, false));
        const auto wrong_type = make_empty_frame(MessageType::RELEASE,
                                                 MessageKind::response, 45U);
        CHECK(!process(server, wrong_type, true) && server.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        ConnectionStateMachine server(ConnectionRole::control_server, 0U);
        CHECK(establish_control(client, server, 0U, 0U));
        const auto unsolicited = make_empty_frame(MessageType::START_STREAM,
                                                  MessageKind::response, 50U);
        CHECK(!process(client, unsolicited, false) && client.poisoned());
    }
    return true;
}

std::vector<std::uint8_t> card_request(MessageType type, std::uint32_t id)
{
    switch (type) {
    case MessageType::CARD_STATUS:
        return make_empty_frame(type, MessageKind::request, id);
    case MessageType::CARD_CONNECT:
        return make_typed_frame(type, MessageKind::request, id,
                                CardConnectRequestPayload{ShareMode::shared});
    case MessageType::CARD_RECONNECT:
        return make_typed_frame(
            type, MessageKind::request, id,
            CardReconnectRequestPayload{1U, ShareMode::shared, Disposition::leave});
    case MessageType::CARD_DISCONNECT:
    case MessageType::END_TRANSACTION:
        return make_typed_frame(
            type, MessageKind::request, id,
            CardDispositionRequestPayload{1U, Disposition::leave});
    case MessageType::CARD_RESET:
    case MessageType::BEGIN_TRANSACTION:
        return make_typed_frame(type, MessageKind::request, id,
                                CardHandleRequestPayload{1U});
    case MessageType::CARD_TRANSMIT:
        {
            const std::array<std::uint8_t, 1U> apdu{0x00U};
            return make_typed_frame(
                type, MessageKind::request, id,
                CardTransmitRequestPayload{1U, ByteView{apdu.data(), apdu.size()}});
        }
    default:
        return {};
    }
}

bool test_capability_gating()
{
    constexpr std::array<MessageType, 8U> card_types{
        MessageType::CARD_STATUS,      MessageType::CARD_CONNECT,
        MessageType::CARD_RECONNECT,   MessageType::CARD_DISCONNECT,
        MessageType::CARD_RESET,       MessageType::CARD_TRANSMIT,
        MessageType::BEGIN_TRANSACTION, MessageType::END_TRANSACTION};
    std::uint32_t id = 100U;
    for (const MessageType type : card_types) {
        ConnectionStateMachine denied_client(ConnectionRole::control_client);
        ConnectionStateMachine denied_server(ConnectionRole::control_server, 0U);
        CHECK(establish_control(denied_client, denied_server, 0U, 0U, id));
        const auto denied_request = card_request(type, id + 1U);
        CHECK(!process(denied_client, denied_request, true));
        CHECK(denied_client.poisoned());
        CHECK(!process(denied_server, denied_request, false));
        CHECK(denied_server.poisoned());

        ConnectionStateMachine allowed_client(ConnectionRole::control_client);
        ConnectionStateMachine allowed_server(ConnectionRole::control_server,
                                              kCapabilityCard);
        CHECK(establish_control(allowed_client, allowed_server, kCapabilityCard,
                                kCapabilityCard, id + 2U));
        const auto allowed_request = card_request(type, id + 3U);
        CHECK(process(allowed_client, allowed_request, true));
        CHECK(process(allowed_server, allowed_request, false));
        id += 4U;
    }
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        ConnectionStateMachine server(ConnectionRole::control_server, 0U);
        CHECK(establish_control(client, server, 0U, 0U));
        const auto stats = make_typed_frame(MessageType::STATS, MessageKind::request,
                                            500U, LeaseRequestPayload{1U});
        CHECK(!process(client, stats, true) && client.poisoned());
        CHECK(!process(server, stats, false) && server.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        ConnectionStateMachine server(ConnectionRole::control_server,
                                      kCapabilityStreamStats);
        CHECK(establish_control(client, server, kCapabilityStreamStats,
                                kCapabilityStreamStats));
        const auto stats = make_typed_frame(MessageType::STATS, MessageKind::request,
                                            501U, LeaseRequestPayload{1U});
        CHECK(process(client, stats, true));
        CHECK(process(server, stats, false));
    }
    {
        ConnectionStateMachine client(ConnectionRole::control_client);
        ConnectionStateMachine server(ConnectionRole::control_server, 0U);
        CHECK(establish_control(client, server, 0U, 0U));
        const auto event = device_event();
        CHECK(!process(server, event, true) && server.poisoned());
        CHECK(!process(client, event, false) && client.poisoned());
    }
    return true;
}

std::vector<std::uint8_t> attach_request(std::uint32_t id)
{
    std::array<std::uint8_t, kNonceLength> nonce{};
    nonce[0] = 0xa5U;
    return make_typed_frame(MessageType::ATTACH_STREAM, MessageKind::request, id,
                            AttachStreamRequestPayload{7U, nonce});
}

bool establish_stream(ConnectionStateMachine& client, ConnectionStateMachine& server,
                      std::uint32_t id = 1U)
{
    const auto request = attach_request(id);
    const auto client_request = process(client, request, true);
    const auto server_request = process(server, request, false);
    if (!client_request || !server_request ||
        server_request.value().event != ConnectionEvent::attach_response_required) {
        return false;
    }
    const auto response = make_empty_frame(MessageType::ATTACH_STREAM,
                                           MessageKind::response, id);
    return process(server, response, true) && process(client, response, false) &&
           client.phase() == ConnectionPhase::active &&
           server.phase() == ConnectionPhase::active;
}

std::vector<std::uint8_t> ts_data(std::uint64_t sequence, std::uint64_t drops,
                                  bool include_packet = false)
{
    std::array<std::uint8_t, 188U> packet{};
    const ByteView data = include_packet ? ByteView{packet.data(), packet.size()} :
                                          ByteView{nullptr, 0U};
    return make_typed_frame(MessageType::TS_DATA, MessageKind::event, 0U,
                            TsDataEventPayload{sequence, drops, data});
}

std::vector<std::uint8_t> stream_end()
{
    return make_typed_frame(
        MessageType::STREAM_END, MessageKind::event, 0U,
        StreamEndEventPayload{CountersPayload{0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U},
                              ErrorCode::OK});
}

class StateConsumer final : public FrameConsumer {
public:
    explicit StateConsumer(ConnectionStateMachine& state) noexcept : state_(state) {}

    Result<void> on_frame(const FrameView& frame) noexcept override
    {
        const auto result = state_.process_inbound(frame);
        if (!result) {
            return Result<void>::failure(result.error());
        }
        ++count_;
        last_event_ = result.value().event;
        return Result<void>::success();
    }

    std::size_t count() const noexcept { return count_; }
    ConnectionEvent last_event() const noexcept { return last_event_; }

private:
    ConnectionStateMachine& state_;
    std::size_t count_ = 0U;
    ConnectionEvent last_event_ = ConnectionEvent::request_accepted;
};

bool test_stream_happy_path_and_framer()
{
    ConnectionStateMachine client(ConnectionRole::stream_client);
    ConnectionStateMachine server(ConnectionRole::stream_server);
    CHECK(establish_stream(client, server, 0x601U));

    const auto first = ts_data(41U, 0U, true);
    CHECK(process(server, first, true));
    std::array<std::uint8_t, 512U> storage{};
    StreamFramer framer(MutableByteView{storage.data(), storage.size()});
    StateConsumer consumer(client);
    for (std::size_t offset = 0U; offset < first.size();) {
        const std::size_t amount = (offset % 17U) + 1U < first.size() - offset ?
            (offset % 17U) + 1U : first.size() - offset;
        const auto fed = framer.feed(ByteView{first.data() + offset, amount}, consumer);
        CHECK(fed);
        offset += amount;
    }
    CHECK(consumer.count() == 1U &&
          consumer.last_event() == ConnectionEvent::ts_data_accepted);

    const auto second = ts_data(42U, 0U);
    CHECK(process(server, second, true));
    CHECK(process(client, second, false));
    const auto end = stream_end();
    CHECK(process(server, end, true));
    CHECK(process(client, end, false));
    CHECK(server.closed() && client.closed());
    CHECK(process(client, second, false).error() == Error::DISCONNECTED);
    CHECK(process(server, second, true).error() == Error::DISCONNECTED);
    return true;
}

bool test_stream_sequence_and_direction_failures()
{
    {
        ConnectionStateMachine client(ConnectionRole::stream_client);
        CHECK(!process(client, hello_request(1U, 0U), true) && client.poisoned());
    }
    {
        ConnectionStateMachine server(ConnectionRole::stream_server);
        CHECK(!process(server, hello_request(1U, 0U), false) && server.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::stream_client);
        ConnectionStateMachine server(ConnectionRole::stream_server);
        CHECK(establish_stream(client, server));
        CHECK(process(client, ts_data(10U, 0U), false));
        CHECK(!process(client, ts_data(12U, 0U), false) && client.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::stream_client);
        ConnectionStateMachine server(ConnectionRole::stream_server);
        CHECK(establish_stream(client, server));
        CHECK(process(client, ts_data(10U, 0U), false));
        CHECK(!process(client, ts_data(10U, 0U), false) && client.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::stream_client);
        ConnectionStateMachine server(ConnectionRole::stream_server);
        CHECK(establish_stream(client, server));
        CHECK(process(client, ts_data(UINT64_MAX, 0U), false));
        CHECK(!process(client, ts_data(0U, 0U), false) && client.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::stream_client);
        ConnectionStateMachine server(ConnectionRole::stream_server);
        CHECK(establish_stream(client, server));
        CHECK(!process(client, ts_data(1U, 1U), false) && client.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::stream_client);
        CHECK(!process(client, ts_data(1U, 0U), false) && client.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::stream_client);
        ConnectionStateMachine server(ConnectionRole::stream_server);
        CHECK(establish_stream(client, server));
        CHECK(!process(client, ts_data(1U, 0U), true) && client.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::stream_client);
        ConnectionStateMachine server(ConnectionRole::stream_server);
        CHECK(establish_stream(client, server));
        CHECK(!process(server, ts_data(1U, 0U), false) && server.poisoned());
    }
    {
        ConnectionStateMachine client(ConnectionRole::stream_client);
        ConnectionStateMachine server(ConnectionRole::stream_server);
        CHECK(establish_stream(client, server));
        CHECK(!process(client, device_event(), false) && client.poisoned());
    }
    return true;
}

}  // namespace

bool run_ipc_state_tests()
{
    return test_control_happy_path_and_interleaved_event() &&
           test_hello_negotiation_and_unknown_capabilities() &&
           test_wrong_order_outstanding_and_correlation() &&
           test_capability_gating() && test_stream_happy_path_and_framer() &&
           test_stream_sequence_and_direction_failures();
}
