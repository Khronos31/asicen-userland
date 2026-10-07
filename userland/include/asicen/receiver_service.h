#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace asicen {

enum class ReceiverState : std::uint8_t {
    Free,
    Leased,
    Streaming,
    Error,
};

struct ReceiverStatus {
    ReceiverState state = ReceiverState::Free;
    std::uint64_t lease_id = 0;
};

class ReceiverLeaseTable final {
public:
    explicit ReceiverLeaseTable(std::size_t receiver_count);

    std::size_t size() const;
    std::optional<std::uint64_t> acquire(std::size_t receiver);
    bool release(std::size_t receiver, std::uint64_t lease_id);
    bool set_streaming(std::size_t receiver, std::uint64_t lease_id, bool streaming);
    bool set_error(std::size_t receiver, std::uint64_t lease_id);
    ReceiverStatus status(std::size_t receiver) const;

private:
    mutable std::mutex mutex_;
    std::vector<ReceiverStatus> receivers_;
    std::uint64_t next_lease_id_ = 1;
};

}  // namespace asicen
