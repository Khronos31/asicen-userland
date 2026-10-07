#include "asicen/receiver_service.h"

namespace asicen {

ReceiverLeaseTable::ReceiverLeaseTable(std::size_t receiver_count)
    : receivers_(receiver_count) {}

std::size_t ReceiverLeaseTable::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return receivers_.size();
}

std::optional<std::uint64_t> ReceiverLeaseTable::acquire(std::size_t receiver) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (receiver >= receivers_.size() || receivers_[receiver].state != ReceiverState::Free) {
        return std::nullopt;
    }

    if (next_lease_id_ == 0) {
        ++next_lease_id_;
    }
    const std::uint64_t id = next_lease_id_++;
    receivers_[receiver] = ReceiverStatus{ReceiverState::Leased, id};
    return id;
}

bool ReceiverLeaseTable::release(std::size_t receiver, std::uint64_t lease_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (receiver >= receivers_.size() || lease_id == 0 ||
        receivers_[receiver].lease_id != lease_id ||
        receivers_[receiver].state == ReceiverState::Free) {
        return false;
    }
    receivers_[receiver] = ReceiverStatus{};
    return true;
}

bool ReceiverLeaseTable::set_streaming(std::size_t receiver,
                                       std::uint64_t lease_id,
                                       bool streaming) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (receiver >= receivers_.size() || lease_id == 0 ||
        receivers_[receiver].lease_id != lease_id ||
        receivers_[receiver].state == ReceiverState::Free ||
        receivers_[receiver].state == ReceiverState::Error) {
        return false;
    }
    receivers_[receiver].state = streaming ? ReceiverState::Streaming
                                           : ReceiverState::Leased;
    return true;
}

bool ReceiverLeaseTable::set_error(std::size_t receiver, std::uint64_t lease_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (receiver >= receivers_.size() || lease_id == 0 ||
        receivers_[receiver].lease_id != lease_id ||
        receivers_[receiver].state == ReceiverState::Free) {
        return false;
    }
    receivers_[receiver].state = ReceiverState::Error;
    return true;
}

ReceiverStatus ReceiverLeaseTable::status(std::size_t receiver) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (receiver >= receivers_.size()) {
        return ReceiverStatus{ReceiverState::Error, 0};
    }
    return receivers_[receiver];
}

}  // namespace asicen
