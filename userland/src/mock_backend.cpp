#include "asicen/backend.h"

#include <algorithm>
#include <array>

namespace asicen {
namespace {

class MockStreamSession final : public StreamSession {
public:
    bool read(std::uint8_t* data,
              std::size_t capacity,
              std::size_t* bytes_read) override {
        if (data == nullptr || bytes_read == nullptr) {
            return false;
        }
        *bytes_read = 0;
        if (capacity < kPacketSize) {
            return true;
        }

        const std::size_t packets = capacity / kPacketSize;
        std::size_t offset = 0;
        for (std::size_t i = 0; i < packets; ++i) {
            std::array<std::uint8_t, kPacketSize> packet{};
            packet.fill(0xff);
            packet[0] = 0x47;
            packet[1] = 0x1f;
            packet[2] = 0xff;
            packet[3] = static_cast<std::uint8_t>(0x10U | (continuity_ & 0x0fU));
            ++continuity_;
            std::copy(packet.begin(), packet.end(), data + offset);
            offset += packet.size();
        }
        *bytes_read = offset;
        return true;
    }

private:
    static constexpr std::size_t kPacketSize = 188;
    std::uint8_t continuity_ = 0;
};

class MockBackend final : public DeviceBackend {
public:
    explicit MockBackend(std::size_t receiver_count)
        : receiver_count_(receiver_count) {}

    std::size_t receiver_count() const override {
        return receiver_count_;
    }

    std::unique_ptr<StreamSession> open_stream(std::size_t receiver) override {
        if (receiver >= receiver_count_) {
            return nullptr;
        }
        return std::make_unique<MockStreamSession>();
    }

private:
    std::size_t receiver_count_;
};

}  // namespace

std::unique_ptr<DeviceBackend> make_mock_backend(std::size_t receiver_count) {
    return std::make_unique<MockBackend>(receiver_count);
}

}  // namespace asicen
