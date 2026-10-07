#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace asicen {

class StreamQueue final {
public:
    explicit StreamQueue(std::size_t capacity_bytes);

    bool push(const std::uint8_t* data, std::size_t size);
    std::vector<std::uint8_t> pop(std::size_t max_bytes);
    void close();

    bool closed() const;
    std::size_t queued_bytes() const;
    std::uint64_t dropped_chunks() const;
    std::uint64_t dropped_bytes() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::vector<std::uint8_t>> chunks_;
    std::size_t capacity_bytes_ = 0;
    std::size_t queued_bytes_ = 0;
    std::uint64_t dropped_chunks_ = 0;
    std::uint64_t dropped_bytes_ = 0;
    bool closed_ = false;
};

}  // namespace asicen
