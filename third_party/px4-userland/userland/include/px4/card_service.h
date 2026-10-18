// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_CARD_SERVICE_H
#define PX4_USERLAND_CARD_SERVICE_H

#include "px4/card.h"
#include "px4/ipc.h"
#include "dtv/userland/detail/card_ownership_engine.h"

#include <cstddef>
#include <cstdint>

namespace px4::userland {

namespace detail = dtv::userland::detail;

using CardClientId = std::uint64_t;

class CardProtocolSession {
public:
    virtual ~CardProtocolSession() noexcept = default;
    virtual Result<void> initialize() noexcept = 0;
    virtual Result<std::size_t> transmit(ByteView apdu,
                                         MutableByteView response) noexcept = 0;
    virtual bool initialized() const noexcept = 0;
    virtual const CardAtr& atr() const noexcept = 0;
    virtual void invalidate() noexcept = 0;
    // Called before daemon workers are drained; implementations may interrupt
    // cancellable protocol waits but must not perform cleanup here.
    virtual void request_stop() noexcept {}
};

#if !defined(PX4_USERLAND_DISABLE_NATIVE_CARD_SESSION)
class NativeCardProtocolSession final : public CardProtocolSession {
public:
    explicit NativeCardProtocolSession(CardSession& session) noexcept : session_(session) {}

    Result<void> initialize() noexcept override { return session_.initialize(); }
    Result<std::size_t> transmit(ByteView apdu,
                                 MutableByteView response) noexcept override
    {
        return session_.transmit(apdu, response);
    }
    bool initialized() const noexcept override { return session_.initialized(); }
    const CardAtr& atr() const noexcept override { return session_.atr(); }
    void invalidate() noexcept override { session_.invalidate(); }

private:
    CardSession& session_;
};
#endif

// Device-1-only backend seam. Implementations perform the Q3U4 backend power
// sequence and UART initialization; CardService never addresses device 2.
class CardServiceBackend {
public:
    virtual ~CardServiceBackend() noexcept = default;
    virtual Result<void> set_power(bool on) noexcept = 0;
    virtual Result<void> initialize_uart() noexcept = 0;
    virtual Result<bool> detect_card() noexcept = 0;
    virtual void request_stop() noexcept {}
};

struct CardServiceStatus final {
    bool present = false;
    bool initialized = false;
    std::uint64_t reader_generation = 1U;
    CardAtr atr{};
};

struct CardServiceConnectResult final {
    std::uint64_t handle = 0U;
    CardAtr atr{};
};

struct CardPresenceChange final {
    bool changed = false;
    bool present = false;
    std::uint64_t reader_generation = 1U;
};

// Socket/USB-independent card ownership service. Calls must be externally
// serialized; px4d's control loop is the single serializer in Increment 8D.
class CardService final {
public:
    CardService(CardServiceBackend& backend, CardProtocolSession& session) noexcept
        : backend_(backend), session_(session), backend_adapter_(backend), session_adapter_(session),
          engine_(backend_adapter_, session_adapter_, ipc::kMaxCardPayload)
    {
    }
    ~CardService() noexcept;

    CardService(const CardService&) = delete;
    CardService& operator=(const CardService&) = delete;

    Result<CardServiceStatus> status() noexcept;
    // Read-only, owned-handle snapshot. Never initializes or rebinds a session.
    Result<CardServiceStatus> handle_status(CardClientId client,
                                           std::uint64_t handle) noexcept;
    Result<CardPresenceChange> poll_presence() noexcept;
    Result<CardServiceConnectResult> connect(
        CardClientId client, ipc::ShareMode share_mode,
        bool generation_bound = false) noexcept;
    Result<CardAtr> reconnect(CardClientId client, std::uint64_t handle,
                              ipc::ShareMode share_mode,
                              ipc::Disposition disposition) noexcept;
    Result<void> disconnect(CardClientId client, std::uint64_t handle,
                            ipc::Disposition disposition) noexcept;
    Result<CardAtr> reset(CardClientId client, std::uint64_t handle) noexcept;
    Result<std::size_t> transmit(CardClientId client, std::uint64_t handle,
                                 ByteView apdu, MutableByteView response) noexcept;
    Result<void> begin_transaction(CardClientId client,
                                   std::uint64_t handle) noexcept;
    Result<void> end_transaction(CardClientId client, std::uint64_t handle,
                                 ipc::Disposition disposition) noexcept;

    // Connection teardown and daemon shutdown are cleanup operations: handles
    // are discarded even if the final device-1 power-off reports an error.
    Result<void> release_connection(CardClientId client) noexcept;
    Result<void> shutdown() noexcept;
    void request_stop() noexcept
    {
        backend_.request_stop();
        session_.request_stop();
    }

    bool has_handles() const noexcept { return engine_.has_handles(); }
    std::size_t handle_count() const noexcept { return engine_.handle_count(); }
    bool powered() const noexcept { return engine_.powered(); }
    std::uint64_t reader_generation() const noexcept { return engine_.reader_generation(); }

private:
    class BackendAdapter final : public detail::CardOwnershipBackend {
    public:
        explicit BackendAdapter(CardServiceBackend& backend) noexcept : backend_(backend) {}
        Result<void> set_power(bool on) noexcept override { return backend_.set_power(on); }
        Result<void> initialize_uart() noexcept override { return backend_.initialize_uart(); }
        Result<bool> detect_card() noexcept override { return backend_.detect_card(); }
    private:
        CardServiceBackend& backend_;
    };

    class SessionAdapter final : public detail::CardOwnershipSession<CardAtr> {
    public:
        explicit SessionAdapter(CardProtocolSession& session) noexcept : session_(session) {}
        Result<void> initialize() noexcept override { return session_.initialize(); }
        Result<std::size_t> transmit(detail::ByteSpan apdu,
                                     detail::MutableByteSpan response) noexcept override
        {
            return session_.transmit(ByteView{apdu.data, apdu.size},
                                     MutableByteView{response.data, response.size});
        }
        bool initialized() const noexcept override { return session_.initialized(); }
        const CardAtr& atr() const noexcept override { return session_.atr(); }
        void invalidate() noexcept override { session_.invalidate(); }
    private:
        CardProtocolSession& session_;
    };

    CardServiceBackend& backend_;
    CardProtocolSession& session_;
    BackendAdapter backend_adapter_;
    SessionAdapter session_adapter_;
    detail::CardOwnershipEngine<CardAtr> engine_;
};

}  // namespace px4::userland

#endif  // PX4_USERLAND_CARD_SERVICE_H
