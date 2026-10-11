// SPDX-License-Identifier: GPL-2.0-only
// ASICEN modification, 2026-10-10: profile-specific wire magic and LIST
// fixtures; every upstream codec assertion is retained in both profiles.
#include "px4/ipc.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#if defined(ASICEN_PROFILE_ASICEN)
#define PX4_TEST_MAGIC "41 53 43 4e "
#define PX4_TEST_LIST_PAYLOAD \
    "01 00 00 00 00 00 00 00 04 00 51 33 55 34 01 03 04 01 " \
    "00 01 00 02 01 01 01 01 02 02 00 02 03 02 01 01"
#define PX4_TEST_LIST_LENGTH "22 00 00 00 "
#else
#define PX4_TEST_MAGIC "50 58 34 55 "
#define PX4_TEST_LIST_PAYLOAD \
    "01 00 00 00 00 00 00 00 04 00 51 33 55 34 01 03 08 01 " \
    "00 01 00 02 01 01 01 02 02 01 02 01 03 01 03 01 " \
    "04 02 00 02 05 02 01 02 06 02 02 01 07 02 03 01"
#define PX4_TEST_LIST_LENGTH "32 00 00 00 "
#endif

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

int hex_nibble(char value) noexcept
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

std::vector<std::uint8_t> bytes(std::string_view hex)
{
    std::vector<std::uint8_t> output;
    int high = -1;
    for (const char character : hex) {
        if (character == ' ' || character == '\n') {
            continue;
        }
        const int nibble = hex_nibble(character);
        if (nibble < 0) {
            return {};
        }
        if (high < 0) {
            high = nibble;
        } else {
            output.push_back(static_cast<std::uint8_t>((high << 4) | nibble));
            high = -1;
        }
    }
    if (high >= 0) {
        return {};
    }
    return output;
}

ByteView view(const std::vector<std::uint8_t>& value) noexcept
{
    return ByteView{value.data(), value.size()};
}

template <typename T>
bool typed_roundtrip(
    ByteView payload, Result<T> (*decode)(ByteView) noexcept,
    Result<std::size_t> (*encode)(const T&, MutableByteView) noexcept)
{
    const auto decoded = decode(payload);
    if (!decoded) {
        return false;
    }
    std::vector<std::uint8_t> encoded(payload.size, 0xa5U);
    const auto result = encode(decoded.value(),
                               MutableByteView{encoded.data(), encoded.size()});
    return result && result.value() == payload.size &&
           (payload.size == 0U ||
            std::memcmp(encoded.data(), payload.data, payload.size) == 0);
}

#define TYPED_ROUNDTRIP(Type, Decoder)                                                       \
    typed_roundtrip<Type>(                                                                  \
        payload, Decoder,                                                                   \
        static_cast<Result<std::size_t> (*)(const Type&, MutableByteView) noexcept>(         \
            &encode_payload))

bool typed_roundtrip_for(MessageType type, MessageKind kind, ByteView payload)
{
    if (kind == MessageKind::error_response) {
        return TYPED_ROUNDTRIP(ErrorResponsePayload, decode_error_response_payload);
    }
    switch (type) {
    case MessageType::HELLO:
        return kind == MessageKind::request ?
                   TYPED_ROUNDTRIP(HelloRequestPayload, decode_hello_request_payload) :
                   TYPED_ROUNDTRIP(HelloResponsePayload, decode_hello_response_payload);
    case MessageType::LIST:
        return kind == MessageKind::request ? static_cast<bool>(decode_empty_payload(payload)) :
                   TYPED_ROUNDTRIP(ListResponsePayload, decode_list_response_payload);
    case MessageType::STATUS:
        return kind == MessageKind::request ? static_cast<bool>(decode_empty_payload(payload)) :
                   TYPED_ROUNDTRIP(StatusResponsePayload, decode_status_response_payload);
    case MessageType::ACQUIRE:
        return kind == MessageKind::request ?
                   TYPED_ROUNDTRIP(AcquireRequestPayload, decode_acquire_request_payload) :
                   TYPED_ROUNDTRIP(AcquireResponsePayload, decode_acquire_response_payload);
    case MessageType::RELEASE:
    case MessageType::START_STREAM:
        return kind == MessageKind::request ?
                   TYPED_ROUNDTRIP(LeaseRequestPayload, decode_lease_request_payload) :
                   static_cast<bool>(decode_empty_payload(payload));
    case MessageType::BEGIN_TRANSACTION:
        return kind == MessageKind::request ?
                   TYPED_ROUNDTRIP(CardHandleRequestPayload,
                                   decode_card_handle_request_payload) :
                   static_cast<bool>(decode_empty_payload(payload));
    case MessageType::TUNE:
        return kind == MessageKind::request ?
                   TYPED_ROUNDTRIP(TuneRequestPayload, decode_tune_request_payload) :
                   TYPED_ROUNDTRIP(TuneResponsePayload, decode_tune_response_payload);
    case MessageType::STOP_STREAM:
    case MessageType::STATS:
        return kind == MessageKind::request ?
                   TYPED_ROUNDTRIP(LeaseRequestPayload, decode_lease_request_payload) :
                   TYPED_ROUNDTRIP(CountersPayload, decode_counters_payload);
    case MessageType::CARD_STATUS:
        return kind == MessageKind::request ? static_cast<bool>(decode_empty_payload(payload)) :
                   TYPED_ROUNDTRIP(CardStatusResponsePayload,
                                   decode_card_status_response_payload);
    case MessageType::CARD_CONNECT:
        return kind == MessageKind::request ?
                   TYPED_ROUNDTRIP(CardConnectRequestPayload,
                                   decode_card_connect_request_payload) :
                   TYPED_ROUNDTRIP(CardConnectResponsePayload,
                                   decode_card_connect_response_payload);
    case MessageType::CARD_RECONNECT:
        return kind == MessageKind::request ?
                   TYPED_ROUNDTRIP(CardReconnectRequestPayload,
                                   decode_card_reconnect_request_payload) :
                   TYPED_ROUNDTRIP(AtrPayload, decode_atr_payload);
    case MessageType::CARD_DISCONNECT:
    case MessageType::END_TRANSACTION:
        return kind == MessageKind::request ?
                   TYPED_ROUNDTRIP(CardDispositionRequestPayload,
                                   decode_card_disposition_request_payload) :
                   static_cast<bool>(decode_empty_payload(payload));
    case MessageType::CARD_RESET:
        return kind == MessageKind::request ?
                   TYPED_ROUNDTRIP(CardHandleRequestPayload,
                                   decode_card_handle_request_payload) :
                   TYPED_ROUNDTRIP(AtrPayload, decode_atr_payload);
    case MessageType::CARD_TRANSMIT:
        return kind == MessageKind::request ?
                   TYPED_ROUNDTRIP(CardTransmitRequestPayload,
                                   decode_card_transmit_request_payload) :
                   TYPED_ROUNDTRIP(CardTransmitResponsePayload,
                                   decode_card_transmit_response_payload);
    case MessageType::ATTACH_STREAM:
        return kind == MessageKind::request ?
                   TYPED_ROUNDTRIP(AttachStreamRequestPayload,
                                   decode_attach_stream_request_payload) :
                   static_cast<bool>(decode_empty_payload(payload));
    case MessageType::TS_DATA:
        return TYPED_ROUNDTRIP(TsDataEventPayload, decode_ts_data_event_payload);
    case MessageType::DEVICE_EVENT:
        return TYPED_ROUNDTRIP(DeviceEventPayload, decode_device_event_payload);
    case MessageType::STREAM_END:
        return TYPED_ROUNDTRIP(StreamEndEventPayload, decode_stream_end_event_payload);
    }
    return false;
}

#undef TYPED_ROUNDTRIP

bool expect_golden(std::string_view hex, MessageType type, MessageKind kind,
                   std::uint32_t request_id)
{
    const auto golden = bytes(hex);
    CHECK(!golden.empty());
    const auto decoded = decode_frame(view(golden));
    CHECK(decoded);
    CHECK(decoded.value().header.major == kProtocolMajor);
    CHECK(decoded.value().header.minor == kProtocolMinor);
    CHECK(decoded.value().header.type == type);
    CHECK(decoded.value().header.kind == kind);
    CHECK(decoded.value().header.request_id == request_id);
    CHECK(decoded.value().header.payload_length == decoded.value().payload.size);
    CHECK(typed_roundtrip_for(type, kind, decoded.value().payload));

    std::vector<std::uint8_t> encoded(golden.size(), 0xa5U);
    const auto result = encode_frame(decoded.value().header, decoded.value().payload,
                                     MutableByteView{encoded.data(), encoded.size()});
    CHECK(result && result.value() == golden.size());
    CHECK(encoded == golden);
    CHECK(!decode_frame(ByteView{golden.data(), golden.size() - 1U}));
    auto trailing = golden;
    trailing.push_back(0U);
    CHECK(!decode_frame(view(trailing)));
    return true;
}

bool test_golden_frames_all_message_ids()
{
    constexpr std::uint32_t id = 0x11223344U;
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 01 00 00 00 44 33 22 11 0c 00 00 00 "
        "01 00 00 00 01 00 00 00 07 00 00 00",
        MessageType::HELLO, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 01 00 01 00 44 33 22 11 08 00 00 00 "
        "01 00 00 00 07 00 00 00",
        MessageType::HELLO, MessageKind::response, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 10 00 03 00 44 33 22 11 0b 00 00 00 "
        "09 00 00 00 05 00 65 72 72 6f 72",
        MessageType::LIST, MessageKind::error_response, id));
    CHECK(expect_golden(PX4_TEST_MAGIC "01 00 00 00 10 00 00 00 44 33 22 11 00 00 00 00",
                        MessageType::LIST, MessageKind::request, id));
    CHECK(expect_golden(PX4_TEST_MAGIC "01 00 00 00 11 00 00 00 44 33 22 11 00 00 00 00",
                        MessageType::STATUS, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 20 00 00 00 44 33 22 11 01 00 00 00 03",
        MessageType::ACQUIRE, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 21 00 00 00 44 33 22 11 08 00 00 00 "
        "08 07 06 05 04 03 02 01",
        MessageType::RELEASE, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 22 00 00 00 44 33 22 11 1e 00 00 00 "
        "08 07 06 05 04 03 02 01 01 01 00 00 00 00 00 00 00 ff ff ff ff "
        "80 8d 5b 00 00 88 13 00 00",
        MessageType::TUNE, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 23 00 00 00 44 33 22 11 08 00 00 00 "
        "08 07 06 05 04 03 02 01",
        MessageType::START_STREAM, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 24 00 00 00 44 33 22 11 08 00 00 00 "
        "08 07 06 05 04 03 02 01",
        MessageType::STOP_STREAM, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 25 00 00 00 44 33 22 11 08 00 00 00 "
        "08 07 06 05 04 03 02 01",
        MessageType::STATS, MessageKind::request, id));
    CHECK(expect_golden(PX4_TEST_MAGIC "01 00 00 00 30 00 00 00 44 33 22 11 00 00 00 00",
                        MessageType::CARD_STATUS, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 31 00 00 00 44 33 22 11 01 00 00 00 01",
        MessageType::CARD_CONNECT, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 32 00 00 00 44 33 22 11 0a 00 00 00 "
        "08 07 06 05 04 03 02 01 02 01",
        MessageType::CARD_RECONNECT, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 33 00 00 00 44 33 22 11 09 00 00 00 "
        "08 07 06 05 04 03 02 01 00",
        MessageType::CARD_DISCONNECT, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 34 00 00 00 44 33 22 11 08 00 00 00 "
        "08 07 06 05 04 03 02 01",
        MessageType::CARD_RESET, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 35 00 00 00 44 33 22 11 11 00 00 00 "
        "08 07 06 05 04 03 02 01 05 00 00 00 90 30 00 00 00",
        MessageType::CARD_TRANSMIT, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 36 00 00 00 44 33 22 11 08 00 00 00 "
        "08 07 06 05 04 03 02 01",
        MessageType::BEGIN_TRANSACTION, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 37 00 00 00 44 33 22 11 09 00 00 00 "
        "08 07 06 05 04 03 02 01 01",
        MessageType::END_TRANSACTION, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 40 00 00 00 44 33 22 11 18 00 00 00 "
        "08 07 06 05 04 03 02 01 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f",
        MessageType::ATTACH_STREAM, MessageKind::request, id));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 40 80 00 00 00 00 00 00 14 00 00 00 "
        "01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00",
        MessageType::TS_DATA, MessageKind::event, 0U));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 f0 80 00 00 00 00 00 00 0b 00 00 00 "
        "01 00 00 00 00 00 00 00 01 01 01",
        MessageType::DEVICE_EVENT, MessageKind::event, 0U));
    CHECK(expect_golden(
        PX4_TEST_MAGIC "01 00 00 00 ff 80 00 00 00 00 00 00 44 00 00 00 "
        "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
        "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
        "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
        "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
        "00 00 00 00",
        MessageType::STREAM_END, MessageKind::event, 0U));
    return true;
}

bool expect_valid_payload(MessageType type, MessageKind kind, std::string_view hex)
{
    const auto payload = bytes(hex);
    if (!hex.empty() && payload.empty()) {
        return false;
    }
    if (!validate_payload(type, kind, view(payload))) {
        return false;
    }
    if (!typed_roundtrip_for(type, kind, view(payload))) {
        return false;
    }
    const std::uint32_t request_id = kind == MessageKind::event ? 0U : 0xabcdef01U;
    const FrameHeader header{kProtocolMajor, kProtocolMinor, type, kind, request_id,
                             static_cast<std::uint32_t>(payload.size())};
    std::vector<std::uint8_t> frame(kFrameHeaderSize + payload.size(), 0U);
    const auto encoded = encode_frame(header, view(payload),
                                      MutableByteView{frame.data(), frame.size()});
    if (!encoded || encoded.value() != frame.size()) {
        return false;
    }
    const auto decoded = decode_frame(view(frame));
    return decoded && decoded.value().header.type == type &&
           decoded.value().header.kind == kind &&
           decoded.value().header.request_id == request_id &&
           decoded.value().payload.size == payload.size() &&
           (payload.empty() ||
            std::memcmp(decoded.value().payload.data, payload.data(), payload.size()) == 0);
}

template <typename T>
bool expect_typed_encoding(const T& value, std::string_view golden_hex)
{
    const auto golden = bytes(golden_hex);
    std::vector<std::uint8_t> output(golden.size(), 0xa5U);
    const auto encoded = encode_payload(value, MutableByteView{output.data(), output.size()});
    return encoded && encoded.value() == golden.size() && output == golden;
}

bool test_typed_encoder_golden_values()
{
    std::array<std::uint8_t, 1U> empty_storage{};
    CHECK(encode_empty_payload(MutableByteView{empty_storage.data(), empty_storage.size()}) &&
          decode_empty_payload(ByteView{nullptr, 0U}));

    CHECK(expect_typed_encoding(HelloRequestPayload{1U, 0U, 1U, 0U, 0x80000007U},
                                "01 00 00 00 01 00 00 00 07 00 00 80"));
    CHECK(expect_typed_encoding(HelloResponsePayload{1U, 0U, 0x80000007U},
                                "01 00 00 00 07 00 00 80"));

    const auto serial = bytes("51 33 55 34");
    const std::array<ReceiverRecord, kReceiverCount> receivers{{
#if defined(ASICEN_PROFILE_ASICEN)
        {0U, 1U, 0U, System::ISDB_S}, {1U, 1U, 1U, System::ISDB_T},
        {2U, 2U, 0U, System::ISDB_S}, {3U, 2U, 1U, System::ISDB_T},
#else
        {0U, 1U, 0U, System::ISDB_S}, {1U, 1U, 1U, System::ISDB_S},
        {2U, 1U, 2U, System::ISDB_T}, {3U, 1U, 3U, System::ISDB_T},
        {4U, 2U, 0U, System::ISDB_S}, {5U, 2U, 1U, System::ISDB_S},
        {6U, 2U, 2U, System::ISDB_T}, {7U, 2U, 3U, System::ISDB_T},
#endif
    }};
#if defined(ASICEN_PROFILE_ASICEN)
    CHECK(expect_typed_encoding(
        ListResponsePayload{1U, view(serial), 1U, 3U, receivers, 4U},
        PX4_TEST_LIST_PAYLOAD));
#else
    CHECK(expect_typed_encoding(
        ListResponsePayload{1U, view(serial), 1U, 3U, receivers},
        PX4_TEST_LIST_PAYLOAD));
#endif

    const std::array<ReceiverState, kReceiverCount> states{{
        ReceiverState::free, ReceiverState::leased, ReceiverState::tuned,
        ReceiverState::streaming, ReceiverState::error, ReceiverState::free,
        ReceiverState::leased, ReceiverState::tuned,
    }};
    CHECK(expect_typed_encoding(
        StatusResponsePayload{1U, 1U, 3U, 1U, 0U, states, 0U, 0U},
        "01 00 00 00 00 00 00 00 01 03 01 00 00 01 02 03 04 00 01 02 "
        "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"));
    CHECK(expect_typed_encoding(AcquireRequestPayload{3U}, "03"));

    std::array<std::uint8_t, kNonceLength> nonce{};
    for (std::size_t index = 0U; index < nonce.size(); ++index) {
        nonce[index] = static_cast<std::uint8_t>(index);
    }
    CHECK(expect_typed_encoding(
        AcquireResponsePayload{0x0102030405060708ULL, nonce},
        "08 07 06 05 04 03 02 01 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f"));
    CHECK(expect_typed_encoding(LeaseRequestPayload{0x0102030405060708ULL},
                                "08 07 06 05 04 03 02 01"));
    CHECK(expect_typed_encoding(CardHandleRequestPayload{0x0102030405060708ULL},
                                "08 07 06 05 04 03 02 01"));
    CHECK(expect_typed_encoding(
        TuneRequestPayload{0x0102030405060708ULL, System::ISDB_T, 1U, 0xffffU,
                           0xffffU, 6000000U, 0U, 5000U},
        "08 07 06 05 04 03 02 01 01 01 00 00 00 00 00 00 00 ff ff ff ff "
        "80 8d 5b 00 00 88 13 00 00"));
    CHECK(expect_typed_encoding(TuneResponsePayload{1U, 0x12345678},
                                "01 78 56 34 12"));
    CHECK(expect_typed_encoding(
        TuneResponsePayload{1U, std::numeric_limits<std::int32_t>::min()},
        "01 00 00 00 80"));

    const CountersPayload counters{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    constexpr std::string_view counters_hex =
        "01 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00 "
        "03 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00 "
        "05 00 00 00 00 00 00 00 06 00 00 00 00 00 00 00 "
        "07 00 00 00 00 00 00 00 08 00 00 00 00 00 00 00";
    CHECK(expect_typed_encoding(counters, counters_hex));

    const auto atr = bytes("3b 00");
    CHECK(expect_typed_encoding(CardStatusResponsePayload{1U, 1U, 1U, view(atr)},
                                "01 01 01 00 00 00 00 00 00 00 02 3b 00"));
    CHECK(expect_typed_encoding(CardConnectRequestPayload{ShareMode::shared}, "01"));
    CHECK(expect_typed_encoding(
        CardConnectResponsePayload{0x0102030405060708ULL, view(atr)},
        "08 07 06 05 04 03 02 01 02 3b 00"));
    CHECK(expect_typed_encoding(
        CardReconnectRequestPayload{0x0102030405060708ULL, ShareMode::exclusive,
                                    Disposition::reset},
        "08 07 06 05 04 03 02 01 02 01"));
    CHECK(expect_typed_encoding(AtrPayload{view(atr)}, "02 3b 00"));
    CHECK(expect_typed_encoding(
        CardDispositionRequestPayload{0x0102030405060708ULL, Disposition::leave},
        "08 07 06 05 04 03 02 01 00"));

    const auto apdu = bytes("90 30 00 00 00");
    CHECK(expect_typed_encoding(
        CardTransmitRequestPayload{0x0102030405060708ULL, view(apdu)},
        "08 07 06 05 04 03 02 01 05 00 00 00 90 30 00 00 00"));
    const auto response = bytes("90 00");
    CHECK(expect_typed_encoding(CardTransmitResponsePayload{view(response)},
                                "02 00 00 00 90 00"));
    CHECK(expect_typed_encoding(
        AttachStreamRequestPayload{0x0102030405060708ULL, nonce},
        "08 07 06 05 04 03 02 01 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f"));
    CHECK(expect_typed_encoding(
        TsDataEventPayload{1U, 0U, ByteView{nullptr, 0U}},
        "01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"));
    CHECK(expect_typed_encoding(
        DeviceEventPayload{1U, DeviceEventKind::attached, EventTargetType::device, 1U},
        "01 00 00 00 00 00 00 00 01 01 01"));
    CHECK(expect_typed_encoding(StreamEndEventPayload{counters, ErrorCode::TIMEOUT},
                                std::string(counters_hex) + " 06 00 00 00"));
    const auto detail = bytes("65 72 72 6f 72");
    CHECK(expect_typed_encoding(ErrorResponsePayload{ErrorCode::PROTOCOL_ERROR, view(detail)},
                                "09 00 00 00 05 00 65 72 72 6f 72"));
    return true;
}

bool test_typed_decoder_values()
{
    auto payload = bytes("01 00 00 00 01 00 00 00 07 00 00 80");
    const auto hello = decode_hello_request_payload(view(payload));
    CHECK(hello && hello.value().min_major == 1U && hello.value().max_major == 1U &&
          hello.value().requested_capabilities == 0x80000007U);

    payload = bytes(
        PX4_TEST_LIST_PAYLOAD);
    const auto list = decode_list_response_payload(view(payload));
#if defined(ASICEN_PROFILE_ASICEN)
    CHECK(list && list.value().generation == 1U && list.value().serial_utf8.size == 4U &&
          list.value().serial_utf8.data[0] == 'Q' &&
          list.value().receivers[3].global_id == 3U &&
          list.value().receivers[3].system == System::ISDB_T);
#else
    CHECK(list && list.value().generation == 1U && list.value().serial_utf8.size == 4U &&
          list.value().serial_utf8.data[0] == 'Q' && list.value().receivers[7].global_id == 7U &&
          list.value().receivers[7].system == System::ISDB_T);
#endif

    payload = bytes(
        "01 00 00 00 00 00 00 00 01 03 01 00 00 01 02 03 04 00 01 02 "
        "08 07 06 05 04 03 02 01 01 02 03 04 05 06 07 08");
    const auto status = decode_status_response_payload(view(payload));
    CHECK(status && status.value().receiver_states[3] == ReceiverState::streaming &&
          status.value().usb_errors == 0x0102030405060708ULL &&
          status.value().protocol_errors == 0x0807060504030201ULL);

    payload = bytes(
        "08 07 06 05 04 03 02 01 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f");
    const auto acquired = decode_acquire_response_payload(view(payload));
    CHECK(acquired && acquired.value().lease_id == 0x0102030405060708ULL &&
          acquired.value().nonce[15] == 0x0fU);

    payload = bytes("08 07 06 05 04 03 02 01");
    const auto lease = decode_lease_request_payload(view(payload));
    const auto handle = decode_card_handle_request_payload(view(payload));
    CHECK(lease && lease.value().lease_id == 0x0102030405060708ULL);
    CHECK(handle && handle.value().card_handle == 0x0102030405060708ULL);

    payload = bytes(
        "08 07 06 05 04 03 02 01 01 01 00 00 00 00 00 00 00 ff ff ff ff "
        "80 8d 5b 00 00 88 13 00 00");
    const auto tune = decode_tune_request_payload(view(payload));
    CHECK(tune && tune.value().lease_id == 0x0102030405060708ULL &&
          tune.value().system == System::ISDB_T && tune.value().frequency_khz == 1U &&
          tune.value().bandwidth_hz == 6000000U && tune.value().timeout_ms == 5000U);

    payload = bytes("01 78 56 34 12");
    const auto tune_result = decode_tune_response_payload(view(payload));
    CHECK(tune_result && tune_result.value().locked == 1U &&
          tune_result.value().cnr_mdb == 0x12345678);
    payload = bytes("01 00 00 00 80");
    const auto unknown_cnr = decode_tune_response_payload(view(payload));
    CHECK(unknown_cnr &&
          unknown_cnr.value().cnr_mdb == std::numeric_limits<std::int32_t>::min());

    payload = bytes(
        "01 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00 "
        "03 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00 "
        "05 00 00 00 00 00 00 00 06 00 00 00 00 00 00 00 "
        "07 00 00 00 00 00 00 00 08 00 00 00 00 00 00 00");
    const auto counters = decode_counters_payload(view(payload));
    CHECK(counters && counters.value().packets == 1U &&
          counters.value().empty_intervals == 8U);

    payload = bytes("01 01 08 07 06 05 04 03 02 01 02 3b 00");
    const auto card_status = decode_card_status_response_payload(view(payload));
    CHECK(card_status && card_status.value().reader_generation == 0x0102030405060708ULL &&
          card_status.value().atr.size == 2U && card_status.value().atr.data[0] == 0x3bU);

    payload = bytes("08 07 06 05 04 03 02 01 02 01");
    const auto reconnect = decode_card_reconnect_request_payload(view(payload));
    CHECK(reconnect && reconnect.value().card_handle == 0x0102030405060708ULL &&
          reconnect.value().share_mode == ShareMode::exclusive &&
          reconnect.value().disposition == Disposition::reset);

    payload = bytes("08 07 06 05 04 03 02 01 05 00 00 00 90 30 00 00 00");
    const auto transmit = decode_card_transmit_request_payload(view(payload));
    CHECK(transmit && transmit.value().card_handle == 0x0102030405060708ULL &&
          transmit.value().apdu.size == 5U && transmit.value().apdu.data[1] == 0x30U);

    payload = bytes(
        "08 07 06 05 04 03 02 01 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f");
    const auto attached = decode_attach_stream_request_payload(view(payload));
    CHECK(attached && attached.value().lease_id == 0x0102030405060708ULL &&
          attached.value().nonce[8] == 8U);

    payload = bytes(
        "08 07 06 05 04 03 02 01 01 00 00 00 00 00 00 00 00 00 00 00");
    const auto ts = decode_ts_data_event_payload(view(payload));
    CHECK(ts && ts.value().sequence == 0x0102030405060708ULL &&
          ts.value().cumulative_drop_count == 1U && ts.value().bytes.size == 0U);

    payload = bytes("08 07 06 05 04 03 02 01 05 02 07");
    const auto event = decode_device_event_payload(view(payload));
    CHECK(event && event.value().generation == 0x0102030405060708ULL &&
          event.value().kind == DeviceEventKind::state_changed &&
          event.value().target_type == EventTargetType::receiver &&
          event.value().target_id == 7U);

    payload = bytes("09 00 00 00 05 00 65 72 72 6f 72");
    const auto error = decode_error_response_payload(view(payload));
    CHECK(error && error.value().error_code == ErrorCode::PROTOCOL_ERROR &&
          error.value().detail_utf8.size == 5U && error.value().detail_utf8.data[4] == 'r');
    return true;
}

bool test_success_and_error_payloads()
{
    CHECK(expect_valid_payload(MessageType::HELLO, MessageKind::response,
                               "01 00 00 00 07 00 00 00"));
    CHECK(expect_valid_payload(MessageType::HELLO, MessageKind::request,
                               "01 00 00 00 01 00 00 00 07 00 00 80"));
    CHECK(expect_valid_payload(MessageType::HELLO, MessageKind::response,
                               "01 00 00 00 07 00 00 80"));
    CHECK(expect_valid_payload(
        MessageType::LIST, MessageKind::response,
        PX4_TEST_LIST_PAYLOAD));
    CHECK(expect_valid_payload(
        MessageType::STATUS, MessageKind::response,
        "01 00 00 00 00 00 00 00 01 03 01 00 00 01 02 03 04 00 01 02 "
        "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"));
    CHECK(expect_valid_payload(MessageType::ACQUIRE, MessageKind::response,
                               "08 07 06 05 04 03 02 01 "
                               "00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f"));
    CHECK(expect_valid_payload(MessageType::RELEASE, MessageKind::response, ""));
    CHECK(expect_valid_payload(MessageType::TUNE, MessageKind::response,
                               "01 78 56 34 12"));
    CHECK(expect_valid_payload(MessageType::START_STREAM, MessageKind::response, ""));
    const std::string_view counters =
        "01 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00 "
        "03 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00 "
        "05 00 00 00 00 00 00 00 06 00 00 00 00 00 00 00 "
        "07 00 00 00 00 00 00 00 08 00 00 00 00 00 00 00";
    CHECK(expect_valid_payload(MessageType::STOP_STREAM, MessageKind::response, counters));
    CHECK(expect_valid_payload(MessageType::STATS, MessageKind::response, counters));
    CHECK(expect_valid_payload(MessageType::CARD_STATUS, MessageKind::response,
                               "01 01 01 00 00 00 00 00 00 00 02 3b 00"));
    CHECK(expect_valid_payload(MessageType::CARD_CONNECT, MessageKind::response,
                               "08 07 06 05 04 03 02 01 02 3b 00"));
    CHECK(expect_valid_payload(MessageType::CARD_RECONNECT, MessageKind::response,
                               "02 3b 00"));
    CHECK(expect_valid_payload(MessageType::CARD_DISCONNECT, MessageKind::response, ""));
    CHECK(expect_valid_payload(MessageType::CARD_RESET, MessageKind::response, "02 3b 00"));
    CHECK(expect_valid_payload(MessageType::CARD_TRANSMIT, MessageKind::response,
                               "02 00 00 00 90 00"));
    CHECK(expect_valid_payload(MessageType::BEGIN_TRANSACTION, MessageKind::response, ""));
    CHECK(expect_valid_payload(MessageType::END_TRANSACTION, MessageKind::response, ""));
    CHECK(expect_valid_payload(MessageType::ATTACH_STREAM, MessageKind::response, ""));

    constexpr std::array<MessageType, 18U> request_types{
        MessageType::HELLO,           MessageType::LIST,
        MessageType::STATUS,          MessageType::ACQUIRE,
        MessageType::RELEASE,         MessageType::TUNE,
        MessageType::START_STREAM,    MessageType::STOP_STREAM,
        MessageType::STATS,           MessageType::CARD_STATUS,
        MessageType::CARD_CONNECT,    MessageType::CARD_RECONNECT,
        MessageType::CARD_DISCONNECT, MessageType::CARD_RESET,
        MessageType::CARD_TRANSMIT,   MessageType::BEGIN_TRANSACTION,
        MessageType::END_TRANSACTION, MessageType::ATTACH_STREAM};
    for (const MessageType type : request_types) {
        CHECK(expect_valid_payload(type, MessageKind::error_response,
                                   "09 00 00 00 05 00 65 72 72 6f 72"));
    }
    return true;
}

std::vector<std::uint8_t> valid_list_frame()
{
    return bytes(
        PX4_TEST_MAGIC "01 00 00 00 10 00 01 00 44 33 22 11 " PX4_TEST_LIST_LENGTH
        PX4_TEST_LIST_PAYLOAD);
}

bool expect_decode_error(const std::vector<std::uint8_t>& frame, Error error)
{
    const auto result = decode_frame(view(frame));
    return !result && result.error() == error;
}

bool test_malformed_headers_and_boundaries()
{
    const auto hello = bytes(
        PX4_TEST_MAGIC "01 00 00 00 01 00 00 00 44 33 22 11 0c 00 00 00 "
        "01 00 00 00 01 00 00 00 07 00 00 00");
    for (std::size_t length = 0U; length < kFrameHeaderSize; ++length) {
        CHECK(!decode_frame(ByteView{hello.data(), length}));
    }
    auto malformed = hello;
    malformed[0] = 'Q';
    CHECK(expect_decode_error(malformed, Error::PROTOCOL_ERROR));
    malformed = hello;
    malformed[4] = 2U;
    CHECK(expect_decode_error(malformed, Error::VERSION_MISMATCH));
    malformed = hello;
    malformed[6] = 1U;
    CHECK(expect_decode_error(malformed, Error::VERSION_MISMATCH));
    malformed = hello;
    malformed[8] = 0xfeU;
    malformed[9] = 0x7fU;
    CHECK(expect_decode_error(malformed, Error::PROTOCOL_ERROR));
    malformed = hello;
    malformed[10] = 0x04U;
    CHECK(expect_decode_error(malformed, Error::PROTOCOL_ERROR));
    malformed = hello;
    malformed[10] = 0x02U;
    CHECK(expect_decode_error(malformed, Error::PROTOCOL_ERROR));
    malformed = hello;
    malformed[12] = malformed[13] = malformed[14] = malformed[15] = 0U;
    CHECK(decode_frame(view(malformed)));  // Zero is valid for request/response IDs.
    malformed = hello;
    malformed[16] = 0x0bU;
    CHECK(expect_decode_error(malformed, Error::PROTOCOL_ERROR));
    malformed = hello;
    malformed.push_back(0U);
    CHECK(expect_decode_error(malformed, Error::PROTOCOL_ERROR));

    auto event = bytes(
        PX4_TEST_MAGIC "01 00 00 00 f0 80 00 00 00 00 00 00 0b 00 00 00 "
        "01 00 00 00 00 00 00 00 01 01 01");
    event[12] = 1U;
    CHECK(expect_decode_error(event, Error::PROTOCOL_ERROR));
    event[12] = 0U;
    event[10] = 1U;
    CHECK(expect_decode_error(event, Error::PROTOCOL_ERROR));

    auto excessive_control = bytes(
        PX4_TEST_MAGIC "01 00 00 00 10 00 00 00 44 33 22 11 01 00 01 00");
    CHECK(expect_decode_error(excessive_control, Error::PROTOCOL_ERROR));
    auto excessive_ts = bytes(
        PX4_TEST_MAGIC "01 00 00 00 40 80 00 00 00 00 00 00 15 00 10 00");
    CHECK(expect_decode_error(excessive_ts, Error::PROTOCOL_ERROR));

    FrameHeader header{kProtocolMajor, kProtocolMinor, MessageType::LIST,
                       MessageKind::request, 1U, 0U};
    std::array<std::uint8_t, kFrameHeaderSize> output{};
    CHECK(encode_frame(header, ByteView{nullptr, 0U},
                       MutableByteView{nullptr, output.size()}).error() ==
          Error::INVALID_ARGUMENT);
    CHECK(encode_frame(header, ByteView{nullptr, 0U},
                       MutableByteView{output.data(), output.size() - 1U}).error() ==
          Error::BUFFER_TOO_SMALL);
    header.payload_length = 1U;
    CHECK(encode_frame(header, ByteView{nullptr, 0U},
                       MutableByteView{output.data(), output.size()}).error() ==
          Error::INVALID_ARGUMENT);
    header.payload_length = 0U;
    header.kind = static_cast<MessageKind>(0xffU);
    CHECK(encode_frame(header, ByteView{nullptr, 0U},
                       MutableByteView{output.data(), output.size()}).error() ==
          Error::INVALID_ARGUMENT);
    return true;
}

bool test_payload_limit_boundaries()
{
    std::vector<std::uint8_t> control(kMaxControlPayload, 'a');
    control[0] = static_cast<std::uint8_t>(ErrorCode::PROTOCOL_ERROR);
    control[1] = control[2] = control[3] = 0U;
    control[4] = 0xfaU;
    control[5] = 0xffU;  // 65,530-byte detail fills the control maximum exactly.
    CHECK(validate_payload(MessageType::LIST, MessageKind::error_response, view(control)));
    control.push_back('a');
    CHECK(!validate_payload(MessageType::LIST, MessageKind::error_response, view(control)));

    constexpr std::size_t max_packet_aligned_ts = kMaxTsBytes - (kMaxTsBytes % 188U);
    std::vector<std::uint8_t> ts(20U + max_packet_aligned_ts, 0U);
    const std::uint32_t max_ts = static_cast<std::uint32_t>(max_packet_aligned_ts);
    ts[16] = static_cast<std::uint8_t>(max_ts);
    ts[17] = static_cast<std::uint8_t>(max_ts >> 8U);
    ts[18] = static_cast<std::uint8_t>(max_ts >> 16U);
    ts[19] = static_cast<std::uint8_t>(max_ts >> 24U);
    CHECK(validate_payload(MessageType::TS_DATA, MessageKind::event, view(ts)));
    ts.resize(kMaxTsDataPayload + 1U, 0U);
    CHECK(!validate_payload(MessageType::TS_DATA, MessageKind::event, view(ts)));

    std::vector<std::uint8_t> card(4U + kMaxCardPayload, 0U);
    card[0] = 0x00U;
    card[1] = 0x10U;
    CHECK(validate_payload(MessageType::CARD_TRANSMIT, MessageKind::response, view(card)));
    card[0] = 0x01U;
    card[1] = 0x10U;
    CHECK(!validate_payload(MessageType::CARD_TRANSMIT, MessageKind::response, view(card)));

    std::vector<std::uint8_t> atr(1U + kMaxAtrLength, 0U);
    atr[0] = static_cast<std::uint8_t>(kMaxAtrLength);
    CHECK(validate_payload(MessageType::CARD_RESET, MessageKind::response, view(atr)));
    atr.push_back(0U);
    atr[0] = static_cast<std::uint8_t>(kMaxAtrLength + 1U);
    CHECK(!validate_payload(MessageType::CARD_RESET, MessageKind::response, view(atr)));
    return true;
}

bool test_response_correlation()
{
    const FrameHeader request{kProtocolMajor, kProtocolMinor, MessageType::STATUS,
                              MessageKind::request, 0U, 0U};
    FrameHeader response{kProtocolMajor, kProtocolMinor, MessageType::STATUS,
                         MessageKind::response, 0U, 36U};
    CHECK(validate_response_to_request(request, response));
    response.kind = MessageKind::error_response;
    CHECK(validate_response_to_request(request, response));
    response.type = MessageType::LIST;
    CHECK(!validate_response_to_request(request, response));
    response.type = request.type;
    response.request_id = 1U;
    CHECK(!validate_response_to_request(request, response));
    response.request_id = request.request_id;
    response.kind = MessageKind::event;
    CHECK(!validate_response_to_request(request, response));
    return true;
}

struct CapturedFrame final {
    FrameHeader header;
    std::vector<std::uint8_t> payload;
};

class RecordingConsumer final : public FrameConsumer {
public:
    Result<void> on_frame(const FrameView& frame) noexcept override
    {
        CapturedFrame captured{frame.header, {}};
        if (frame.payload.size != 0U) {
            captured.payload.assign(frame.payload.data,
                                    frame.payload.data + frame.payload.size);
        }
        frames.push_back(std::move(captured));
        return Result<void>::success();
    }

    std::vector<CapturedFrame> frames;
};

bool test_stream_framer_fragmentation_and_coalescing()
{
    const auto hello = bytes(
        PX4_TEST_MAGIC "01 00 00 00 01 00 00 00 44 33 22 11 0c 00 00 00 "
        "01 00 00 00 01 00 00 00 07 00 00 00");
    const auto list = bytes(
        PX4_TEST_MAGIC "01 00 00 00 10 00 00 00 45 33 22 11 00 00 00 00");

    for (std::size_t split = 0U; split <= hello.size(); ++split) {
        std::array<std::uint8_t, 64U> storage{};
        StreamFramer framer(MutableByteView{storage.data(), storage.size()});
        RecordingConsumer consumer;
        const auto first = framer.feed(ByteView{hello.data(), split}, consumer);
        CHECK(first);
        const auto second = framer.feed(
            ByteView{hello.data() + split, hello.size() - split}, consumer);
        CHECK(second);
        CHECK(first.value() + second.value() == 1U);
        CHECK(consumer.frames.size() == 1U);
        CHECK(consumer.frames[0].header.type == MessageType::HELLO);
        CHECK(framer.buffered_size() == 0U && !framer.failed());
    }

    {
        std::array<std::uint8_t, 64U> storage{};
        StreamFramer framer(MutableByteView{storage.data(), storage.size()});
        RecordingConsumer consumer;
        std::size_t completed = 0U;
        for (std::size_t index = 0U; index < hello.size(); ++index) {
            const auto result = framer.feed(ByteView{hello.data() + index, 1U}, consumer);
            CHECK(result);
            completed += result.value();
        }
        CHECK(completed == 1U && consumer.frames.size() == 1U);
    }

    std::vector<std::uint8_t> coalesced = hello;
    coalesced.insert(coalesced.end(), list.begin(), list.end());
    {
        std::array<std::uint8_t, 64U> storage{};
        StreamFramer framer(MutableByteView{storage.data(), storage.size()});
        RecordingConsumer consumer;
        const auto result = framer.feed(view(coalesced), consumer);
        CHECK(result && result.value() == 2U);
        CHECK(consumer.frames.size() == 2U);
        CHECK(consumer.frames[0].header.type == MessageType::HELLO);
        CHECK(consumer.frames[1].header.type == MessageType::LIST);
    }

    {
        std::array<std::uint8_t, 64U> storage{};
        StreamFramer framer(MutableByteView{storage.data(), storage.size()});
        RecordingConsumer consumer;
        const std::size_t crossing = hello.size() + 7U;
        const auto first = framer.feed(ByteView{coalesced.data(), crossing}, consumer);
        CHECK(first && first.value() == 1U);
        CHECK(framer.buffered_size() == 7U);
        const auto second = framer.feed(
            ByteView{coalesced.data() + crossing, coalesced.size() - crossing}, consumer);
        CHECK(second && second.value() == 1U);
        CHECK(consumer.frames.size() == 2U && framer.buffered_size() == 0U);
    }

    {
        std::array<std::uint8_t, 64U> storage{};
        StreamFramer framer(MutableByteView{storage.data(), storage.size()});
        RecordingConsumer consumer;
        const auto partial =
            framer.feed(ByteView{hello.data(), hello.size() - 1U}, consumer);
        CHECK(partial && partial.value() == 0U);
        CHECK(framer.buffered_size() == hello.size() - 1U);
        const auto completed =
            framer.feed(ByteView{hello.data() + hello.size() - 1U, 1U}, consumer);
        CHECK(completed && completed.value() == 1U);
    }
    return true;
}

bool test_stream_framer_early_rejection_and_reset()
{
    const auto valid = bytes(
        PX4_TEST_MAGIC "01 00 00 00 10 00 00 00 44 33 22 11 00 00 00 00");
    const auto oversized = bytes(
        PX4_TEST_MAGIC "01 00 00 00 10 00 00 00 44 33 22 11 01 00 01 00");
    std::array<std::uint8_t, 128U> storage{};
    StreamFramer framer(MutableByteView{storage.data(), storage.size()});
    RecordingConsumer consumer;

    const auto prefix = framer.feed(ByteView{oversized.data(), 19U}, consumer);
    CHECK(prefix && prefix.value() == 0U && !framer.failed());
    const auto rejected = framer.feed(ByteView{oversized.data() + 19U, 1U}, consumer);
    CHECK(!rejected && rejected.error() == Error::PROTOCOL_ERROR);
    CHECK(framer.failed() && framer.failure() == Error::PROTOCOL_ERROR);
    CHECK(framer.buffered_size() == kFrameHeaderSize);
    CHECK(framer.feed(view(valid), consumer).error() == Error::PROTOCOL_ERROR);

    framer.reset();
    CHECK(!framer.failed() && framer.buffered_size() == 0U);
    const auto recovered = framer.feed(view(valid), consumer);
    CHECK(recovered && recovered.value() == 1U);

    auto malformed_body = bytes(
        PX4_TEST_MAGIC "01 00 00 00 01 00 00 00 44 33 22 11 0c 00 00 00 "
        "02 00 00 00 01 00 00 00 07 00 00 00");
    const auto malformed = framer.feed(view(malformed_body), consumer);
    CHECK(!malformed && malformed.error() == Error::PROTOCOL_ERROR && framer.failed());
    framer.reset();
    CHECK(framer.feed(view(valid), consumer));

    std::array<std::uint8_t, 24U> small_storage{};
    StreamFramer small(MutableByteView{small_storage.data(), small_storage.size()});
    const auto no_capacity = small.feed(
        ByteView{malformed_body.data(), kFrameHeaderSize}, consumer);
    CHECK(!no_capacity && no_capacity.error() == Error::BUFFER_TOO_SMALL);
    CHECK(small.failed());
    small.reset();
    CHECK(!small.failed());

    StreamFramer invalid_storage(MutableByteView{nullptr, 64U});
    CHECK(invalid_storage.feed(view(valid), consumer).error() == Error::INVALID_ARGUMENT);
    CHECK(!invalid_storage.failed());
    return true;
}

bool test_malformed_payloads()
{
    CHECK(!expect_valid_payload(MessageType::HELLO, MessageKind::request,
                                "02 00 00 00 01 00 00 00 00 00 00 00"));
    CHECK(!expect_valid_payload(MessageType::HELLO, MessageKind::response,
                                "01 00 01 00 00 00 00 00"));
    auto list = valid_list_frame();
    CHECK(decode_frame(view(list)));
    list[20U + 8U + 2U] = 0xc0U;  // Invalid UTF-8 serial.
    CHECK(expect_decode_error(list, Error::PROTOCOL_ERROR));
    list = valid_list_frame();
    list[20U + 8U + 2U] = 0U;  // Embedded NUL.
    CHECK(expect_decode_error(list, Error::PROTOCOL_ERROR));
    list = valid_list_frame();
    list[20U + 8U + 2U + 4U + 2U] = 7U;  // receiver_count
    CHECK(expect_decode_error(list, Error::PROTOCOL_ERROR));
    list = valid_list_frame();
    list[20U + 8U + 2U + 4U + 1U] = 0x80U;  // reserved USB mask
    CHECK(expect_decode_error(list, Error::PROTOCOL_ERROR));
    list = valid_list_frame();
    list.back() = 2U;  // receiver 7 system disagrees with fixed layout.
    CHECK(expect_decode_error(list, Error::PROTOCOL_ERROR));

    CHECK(!expect_valid_payload(MessageType::STATUS, MessageKind::response,
                                "00 00 00 00 00 00 00 00 02 00 00 00 "
                                "00 00 00 00 00 00 00 00 "
                                "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"));
    CHECK(!expect_valid_payload(MessageType::ACQUIRE, MessageKind::request, "08"));
    CHECK(!expect_valid_payload(MessageType::TUNE, MessageKind::request,
                                "00 00 00 00 00 00 00 00 03 00 00 00 00 00 00 00 00 "
                                "ff ff ff ff 80 8d 5b 00 00 88 13 00 00"));
    const auto semantic_tune = bytes(
        "00 00 00 00 00 00 00 00 01 50 c3 00 00 00 00 00 00 "
        "ff ff ff ff 00 00 00 00 00 63 00 00 00");
    // Semantic-invalid fields are deliberately accepted by the wire codec;
    // TunerService returns INVALID_ARGUMENT while keeping the connection.
    CHECK(decode_tune_request_payload(view(semantic_tune)));
    CHECK(validate_payload(MessageType::TUNE, MessageKind::request,
                           view(semantic_tune)));
    const auto wire_invalid_lnb = bytes(
        "00 00 00 00 00 00 00 00 01 50 c3 00 00 00 00 00 00 "
        "ff ff ff ff 00 00 00 00 01 64 00 00 00");
    CHECK(!decode_tune_request_payload(view(wire_invalid_lnb)));
    CHECK(!expect_valid_payload(MessageType::CARD_CONNECT, MessageKind::request, "03"));
    CHECK(!expect_valid_payload(MessageType::CARD_RECONNECT, MessageKind::request,
                                "00 00 00 00 00 00 00 00 01 02"));
    CHECK(!expect_valid_payload(MessageType::CARD_RESET, MessageKind::response,
                                "22 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
                                "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"));
    CHECK(!expect_valid_payload(MessageType::CARD_TRANSMIT, MessageKind::response,
                                "01 10 00 00"));
    CHECK(!expect_valid_payload(MessageType::CARD_TRANSMIT, MessageKind::response,
                                "02 00 00 00 90"));
    CHECK(!expect_valid_payload(MessageType::TS_DATA, MessageKind::event,
                                "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 bd 00 00 00"));
    CHECK(!expect_valid_payload(MessageType::DEVICE_EVENT, MessageKind::event,
                                "00 00 00 00 00 00 00 00 06 01 01"));
    CHECK(!expect_valid_payload(MessageType::DEVICE_EVENT, MessageKind::event,
                                "00 00 00 00 00 00 00 00 01 04 00"));

    CHECK(!expect_valid_payload(MessageType::LIST, MessageKind::error_response,
                                "00 00 00 00 00 00"));
    CHECK(!expect_valid_payload(MessageType::LIST, MessageKind::error_response,
                                "fe 00 00 00 00 00"));
    CHECK(!expect_valid_payload(MessageType::LIST, MessageKind::error_response,
                                "09 00 00 00 01 00 00"));
    CHECK(!expect_valid_payload(MessageType::LIST, MessageKind::error_response,
                                "09 00 00 00 02 00 c0 80"));
    CHECK(!expect_valid_payload(MessageType::LIST, MessageKind::error_response,
                                "09 00 00 00 01 00 61 00"));
    CHECK(!validate_payload(static_cast<MessageType>(0x7ffeU), MessageKind::request,
                            ByteView{nullptr, 0U}));
    return true;
}

bool test_constants_and_fixed_enums()
{
    static_assert(kFrameHeaderSize == 20U);
    static_assert(kMaxControlPayload == 65536U);
    static_assert(kMaxTsBytes == 1048576U);
    static_assert(kMaxTsDataPayload == 1048596U);
    static_assert(kMaxCardPayload == 4096U);
    static_assert(kMaxAtrLength == 33U);
    static_assert(static_cast<std::uint16_t>(MessageType::HELLO) == 0x0001U);
    static_assert(static_cast<std::uint16_t>(MessageType::STREAM_END) == 0x80ffU);
    static_assert(static_cast<std::uint32_t>(ErrorCode::OK) == 0U);
    static_assert(static_cast<std::uint32_t>(ErrorCode::SLOW_CONSUMER) == 15U);
    static_assert(static_cast<std::uint32_t>(ErrorCode::INTERNAL) == 255U);
    CHECK(is_known_error_code(ErrorCode::OK));
    CHECK(is_known_error_code(ErrorCode::INTERNAL));
    CHECK(!is_known_error_code(static_cast<ErrorCode>(16U)));
    CHECK(payload_limit(MessageType::LIST) == kMaxControlPayload);
    CHECK(payload_limit(MessageType::TS_DATA) == kMaxTsDataPayload);
    return true;
}

}  // namespace

bool run_ipc_tests()
{
    return test_constants_and_fixed_enums() && test_golden_frames_all_message_ids() &&
           test_typed_encoder_golden_values() && test_typed_decoder_values() &&
           test_success_and_error_payloads() && test_malformed_headers_and_boundaries() &&
           test_malformed_payloads() && test_payload_limit_boundaries() &&
           test_response_correlation() &&
           test_stream_framer_fragmentation_and_coalescing() &&
           test_stream_framer_early_rejection_and_reset();
}
