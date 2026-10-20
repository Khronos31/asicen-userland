// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded synchronous UDTV requests for the existing product IPC proofs.
#ifndef ASICEN_TEST_WIRE_CLIENT_H
#define ASICEN_TEST_WIRE_CLIENT_H
#include "px4/posix_ipc.h"
#include <dtv/userland/records.h>
#include <chrono>
#include <memory>
#include <vector>
namespace asicen::test {
namespace wire = dtv::userland::wire;
using namespace px4::userland;
using namespace px4::userland::ipc::posix;
struct Reply final {
    wire::Header header{};
    std::vector<std::uint8_t> payload;
};
class WireClient final : private wire::FrameHandler {
public:
    static std::unique_ptr<WireClient> connect(const EndpointConfig& endpoint)
    {
        auto stream = SocketStream::connect(endpoint, Timeout{2000U});
        if (!stream) return nullptr;
        auto client = std::unique_ptr<WireClient>(new WireClient(std::move(stream.value())));
        wire::HelloRequest hello_request{};
        hello_request.min = wire::Version{1U, 0U};
        hello_request.max = wire::Version{1U, 0U};
        const auto hello = client->request(wire::kOpcodeHello, hello_request,
                                           wire::encode_hello_request);
        if (!hello) return nullptr;
        const auto decoded = wire::decode_hello_response(
            ByteView{hello.value().payload.data(), hello.value().payload.size()});
        if (!decoded) return nullptr;
        client->epoch = decoded.value().epoch;
        return client;
    }
    template <typename Record>
    Result<Reply> request(std::uint16_t opcode, const Record& value,
        Result<std::size_t> (*encode)(const Record&, MutableByteView) noexcept)
    {
        std::vector<std::uint8_t> payload(wire::kMaxControlPayload);
        const auto size = encode(value, MutableByteView{payload.data(), payload.size()});
        if (!size) return Result<Reply>::failure(size.error());
        wire::Header header{};
        header.kind = wire::Kind::request;
        header.opcode = opcode;
        header.request_id = ++request_id_;
        header.payload_length = static_cast<std::uint32_t>(size.value());
        std::vector<std::uint8_t> frame(wire::kHeaderSize + size.value());
        const auto framed = wire::encode_frame(header, ByteView{payload.data(), size.value()},
                                               MutableByteView{frame.data(), frame.size()});
        if (!framed) return Result<Reply>::failure(framed.error());
        const auto written = stream_.write_all(ByteView{frame.data(), framed.value()},
                                                 Timeout{2000U});
        if (!written) return Result<Reply>::failure(written.error());
        received_ = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!received_ && std::chrono::steady_clock::now() < deadline) {
            std::uint8_t bytes[4096U]{};
            const auto read = stream_.read_some(MutableByteView{bytes, sizeof(bytes)}, Timeout{100U});
            if (!read) {
                if (read.error() == Error::TIMEOUT) continue;
                return Result<Reply>::failure(read.error());
            }
            const auto decoded = receiver_.feed(ByteView{bytes, read.value()}, *this);
            if (!decoded) return Result<Reply>::failure(decoded.error());
        }
        if (!received_) return Result<Reply>::failure(Error::TIMEOUT);
        if (reply_.header.kind == wire::Kind::error_response) {
            const auto error = wire::decode_error_response(
                ByteView{reply_.payload.data(), reply_.payload.size()});
            return Result<Reply>::failure(error ? static_cast<Error>(error.value().error.code)
                                                : Error::PROTOCOL_ERROR);
        }
        if (reply_.header.opcode != opcode || reply_.header.kind != wire::Kind::response)
            return Result<Reply>::failure(Error::PROTOCOL_ERROR);
        return Result<Reply>::success(std::move(reply_));
    }
    void close() noexcept { stream_.close(); }
    wire::Epoch epoch{};
private:
    explicit WireClient(SocketStream stream)
        : stream_(std::move(stream)), storage_(wire::kMaxControlPayload),
          receiver_(MutableByteView{storage_.data(), storage_.size()}) {}
    Result<void> on_frame(const wire::Frame& frame) noexcept override
    {
        if (frame.header.kind == wire::Kind::event) return Result<void>::success();
        if (frame.header.request_id != request_id_) return Result<void>::failure(Error::PROTOCOL_ERROR);
        reply_.header = frame.header;
        reply_.payload.clear();
        if (frame.payload.size != 0U)
            reply_.payload.assign(frame.payload.data, frame.payload.data + frame.payload.size);
        received_ = true;
        return Result<void>::success();
    }
    SocketStream stream_;
    std::vector<std::uint8_t> storage_;
    wire::FrameReceiver receiver_;
    Reply reply_;
    std::uint32_t request_id_ = 0U;
    bool received_ = false;
};
} // namespace asicen::test
#endif
