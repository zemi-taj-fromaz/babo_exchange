#pragma once

#include "book/order_listener.h"
#include "core/order_identity.hpp"
#include "egress/client_order_event.hpp"

#include <rigtorp/SPSCQueue.h>

#include <cstdint>
#include <unordered_map>

namespace babo::egress {

using ClientOrderEventQueue = rigtorp::SPSCQueue<ClientOrderEvent>;

// Engine-thread-only adapter from canonical matching events to private client
// notifications. Bitstamp ids are ignored; a client-vs-client fill emits two
// queue entries, while a client-vs-Bitstamp fill emits only one.
class ClientOrderListener final : public book::OrderListener<std::uint64_t> {
public:
    explicit ClientOrderListener(ClientOrderEventQueue& events) noexcept;

    // Call on the engine thread before book.add(). Returns false for an invalid
    // namespace or a duplicate exchange order id.
    bool trackClientOrder(core::ExchangeOrderId exchangeOrderId,
                          core::SessionId sessionId,
                          core::ClientOrderId clientOrderId,
                          std::uint64_t priceTicks,
                          std::uint64_t qtyLots);

    [[nodiscard]] bool ownsClientOrder(core::ExchangeOrderId exchangeOrderId,
                                       core::SessionId sessionId) const noexcept;
    void emitCancelRejected(core::SessionId sessionId,
                            core::ExchangeOrderId exchangeOrderId,
                            RejectReason reason);

    void on_accept(const std::uint64_t& orderId) override;
    void on_reject(const std::uint64_t& orderId, const char* reason) override;
    void on_fill(const book::Fill<std::uint64_t>& fill) override;
    void on_cancel(const std::uint64_t& orderId) override;
    void on_cancel_reject(const std::uint64_t& orderId,
                          const char* reason) override;
    void on_replace(const std::uint64_t& orderId,
                    const std::int64_t& sizeDelta,
                    std::uint64_t newPrice) override;
    void on_replace_reject(const std::uint64_t& orderId,
                           const char* reason) override;

private:
    struct ClientOrderState {
        core::SessionId session_id{};
        core::ClientOrderId client_order_id{};
        std::uint64_t price_ticks{};
        std::uint64_t remaining_qty_lots{};
    };

    using Registry =
        std::unordered_map<core::ExchangeOrderId, ClientOrderState>;

    ClientOrderEvent makeEvent(ClientOrderEventType type,
                               core::ExchangeOrderId orderId,
                               const ClientOrderState& state);
    void emitFill(core::ExchangeOrderId orderId, FillRole role,
                  const book::Fill<std::uint64_t>& fill);
    static RejectReason mapRejectReason(const char* reason) noexcept;

    ClientOrderEventQueue& events_;
    Registry client_orders_;
    std::uint64_t next_event_sequence_ = 1;
};

} // namespace babo::egress
