// SPDX-License-Identifier: GPL-2.0-only
#include "px4/card_service.h"

#include <string>
#include <vector>

namespace {
using namespace px4::userland;

class Backend final : public CardServiceBackend {
public:
    explicit Backend(std::vector<std::string>& events) : events_(events) {}
    Result<void> set_power(bool on) noexcept override
    {
        events_.emplace_back(on ? "power-on" : "power-off");
        return Result<void>::success();
    }
    Result<void> initialize_uart() noexcept override
    {
        events_.emplace_back("uart");
        return Result<void>::success();
    }
    Result<bool> detect_card() noexcept override
    {
        events_.emplace_back("detect");
        return Result<bool>::success(true);
    }
    void request_stop() noexcept override { events_.emplace_back("backend-stop"); }

private:
    std::vector<std::string>& events_;
};

class Session final : public CardProtocolSession {
public:
    explicit Session(std::vector<std::string>& events) : events_(events) {}
    Result<void> initialize() noexcept override
    {
        events_.emplace_back("initialize");
        initialized_ = true;
        return Result<void>::success();
    }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    {
        return Result<std::size_t>::success(0U);
    }
    bool initialized() const noexcept override { return initialized_; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override
    {
        events_.emplace_back("invalidate");
        initialized_ = false;
    }
    void request_stop() noexcept override { events_.emplace_back("session-stop"); }

private:
    std::vector<std::string>& events_;
    CardAtr atr_{};
    bool initialized_ = false;
};
}  // namespace

int main()
{
    std::vector<std::string> events;
    Backend backend(events);
    Session session(events);
    CardService service(backend, session);
    if (!service.connect(1U, ipc::ShareMode::shared)) return 1;

    const std::vector<std::string> before_stop{"power-on", "uart", "initialize"};
    if (events != before_stop) return 2;
    service.request_stop();
    const std::vector<std::string> after_stop{
        "power-on", "uart", "initialize", "backend-stop", "session-stop"};
    if (events != after_stop || !service.powered() || !service.has_handles()) return 3;

    if (!service.shutdown()) return 4;
    const std::vector<std::string> after_shutdown{
        "power-on", "uart", "initialize", "backend-stop", "session-stop",
        "invalidate", "power-off"};
    return events == after_shutdown && !service.powered() && !service.has_handles() ? 0 : 5;
}
