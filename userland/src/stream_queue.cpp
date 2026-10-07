#include "asicen/stream_queue.h"

#include <algorithm>

namespace asicen {

StreamQueue::StreamQueue(std::size_t capacity_bytes)
    : capacity_bytes_(capacity_bytes) {}

bool StreamQueue::push(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || size > capacity_bytes_ ||
        queued_bytes_ > capacity_bytes_ - size) {
        ++dropped_chunks_;
        dropped_bytes_ += size;
        return false;
    }

    chunks_.emplace_back(data, data + size);
    queued_bytes_ += size;
    condition_.notify_one();
    return true;
}

std::vector<std::uint8_t> StreamQueue::pop(std::size_t max_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (chunks_.empty() || max_bytes == 0) {
        return {};
    }

    std::vector<std::uint8_t> result;
    result.reserve(std::min(max_bytes, queued_bytes_));
    while (!chunks_.empty() && result.size() < max_bytes) {
        auto& front = chunks_.front();
        const std::size_t take = std::min(max_bytes - result.size(), front.size());
        result.insert(result.end(), front.begin(), front.begin() + static_cast<std::ptrdiff_t>(take));
        queued_bytes_ -= take;
        if (take == front.size()) {
            chunks_.pop_front();
        } else {
            front.erase(front.begin(), front.begin() + static_cast<std::ptrdiff_t>(take));
        }
    }
    return result;
}

void StreamQueue::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    condition_.notify_all();
}

bool StreamQueue::closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

std::size_t StreamQueue::queued_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queued_bytes_;
}

std::uint64_t StreamQueue::dropped_chunks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_chunks_;
}

std::uint64_t StreamQueue::dropped_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_bytes_;
}

}  // namespace asicen
