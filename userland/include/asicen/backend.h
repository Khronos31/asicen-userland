#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace asicen {

class StreamSession {
public:
    virtual ~StreamSession() = default;

    // Returns false on terminal backend failure. A successful call may return
    // zero bytes when no data is currently available.
    virtual bool read(std::uint8_t* data,
                      std::size_t capacity,
                      std::size_t* bytes_read) = 0;
};

class DeviceBackend {
public:
    virtual ~DeviceBackend() = default;

    virtual std::size_t receiver_count() const = 0;
    virtual std::unique_ptr<StreamSession> open_stream(std::size_t receiver) = 0;
};

std::unique_ptr<DeviceBackend> make_mock_backend(std::size_t receiver_count);

}  // namespace asicen
