#include "egress/client_order_listener.hpp"

#include <algorithm>
#include <string_view>

namespace babo::egress {

ClientOrderListener::ClientOrderListener(ClientOrderEventQueue& events) noexcept
    : events_(events) {}

bool ClientOrderListener::trackClientOrder(
    core::ExchangeOrderId exchangeOrderId, core::SessionId sessionId,
    core::ClientOrderId clientOrderId, std::uint64_t priceTicks,
    std::uint64_t qtyLots) {
    if (!core::isClientOrderId(exchangeOrderId) || sessionId == 0) {
        return false;
    }
    return client_orders_
        .try_emplace(exchangeOrderId,
                     ClientOrderState{sessionId, clientOrderId, priceTicks,
                                      qtyLots})
        .second;
}

bool ClientOrderListener::ownsClientOrder(
    core::ExchangeOrderId exchangeOrderId,
    core::SessionId sessionId) const noexcept {
    const auto it = client_orders_.find(exchangeOrderId);
    return it != client_orders_.end() && it->second.session_id == sessionId;
}

void ClientOrderListener::emitCancelRejected(
    core::SessionId sessionId, core::ExchangeOrderId exchangeOrderId,
    RejectReason reason) {
    ClientOrderState state;
    state.session_id = sessionId;
    auto event =
        makeEvent(ClientOrderEventType::CancelRejected, exchangeOrderId, state);
    event.reject_reason = reason;
    events_.push(event);
}

ClientOrderEvent ClientOrderListener::makeEvent(
    ClientOrderEventType type, core::ExchangeOrderId orderId,
    const ClientOrderState& state) {
    ClientOrderEvent event;
    event.type = type;
    event.event_sequence = next_event_sequence_++;
    event.target_session_id = state.session_id;
    event.client_order_id = state.client_order_id;
    event.exchange_order_id = orderId;
    event.price_ticks = state.price_ticks;
    event.remaining_qty_lots = state.remaining_qty_lots;
    return event;
}

void ClientOrderListener::on_accept(const std::uint64_t& orderId) {
    const auto it = client_orders_.find(orderId);
    if (it == client_orders_.end()) {
        return;
    }
    auto event = makeEvent(ClientOrderEventType::Accepted, orderId, it->second);
    event.qty_lots = it->second.remaining_qty_lots;
    events_.push(event);
}

void ClientOrderListener::on_reject(const std::uint64_t& orderId,
                                    const char* reason) {
    const auto it = client_orders_.find(orderId);
    if (it == client_orders_.end()) {
        return;
    }
    auto event = makeEvent(ClientOrderEventType::Rejected, orderId, it->second);
    event.reject_reason = mapRejectReason(reason);
    events_.push(event);
    client_orders_.erase(it);
}

void ClientOrderListener::on_fill(const book::Fill<std::uint64_t>& fill) {
    emitFill(fill.maker_id, FillRole::Maker, fill);
    emitFill(fill.taker_id, FillRole::Taker, fill);
}

void ClientOrderListener::emitFill(core::ExchangeOrderId orderId, FillRole role,
                                   const book::Fill<std::uint64_t>& fill) {
    if (!core::isClientOrderId(orderId)) {
        return;
    }
    const auto it = client_orders_.find(orderId);
    if (it == client_orders_.end()) {
        return;
    }

    auto& state = it->second;
    state.remaining_qty_lots -=
        std::min(state.remaining_qty_lots, fill.qty);

    auto event = makeEvent(ClientOrderEventType::Fill, orderId, state);
    event.price_ticks = fill.price;
    event.qty_lots = fill.qty;
    event.fill_role = role;
    events_.push(event);

    if (state.remaining_qty_lots == 0) {
        client_orders_.erase(it);
    }
}

void ClientOrderListener::on_cancel(const std::uint64_t& orderId) {
    const auto it = client_orders_.find(orderId);
    if (it == client_orders_.end()) {
        return;
    }
    auto event = makeEvent(ClientOrderEventType::Cancelled, orderId, it->second);
    event.qty_lots = it->second.remaining_qty_lots;
    event.remaining_qty_lots = 0;
    events_.push(event);
    client_orders_.erase(it);
}

void ClientOrderListener::on_cancel_reject(const std::uint64_t& orderId,
                                           const char* reason) {
    const auto it = client_orders_.find(orderId);
    if (it == client_orders_.end()) {
        return;
    }
    auto event =
        makeEvent(ClientOrderEventType::CancelRejected, orderId, it->second);
    event.reject_reason = mapRejectReason(reason);
    events_.push(event);
}

void ClientOrderListener::on_replace(const std::uint64_t& orderId,
                                     const std::int64_t& sizeDelta,
                                     std::uint64_t newPrice) {
    const auto it = client_orders_.find(orderId);
    if (it == client_orders_.end()) {
        return;
    }

    auto& state = it->second;
    if (sizeDelta < 0) {
        const auto reduction = static_cast<std::uint64_t>(-sizeDelta);
        state.remaining_qty_lots -=
            std::min(state.remaining_qty_lots, reduction);
    } else {
        state.remaining_qty_lots += static_cast<std::uint64_t>(sizeDelta);
    }
    state.price_ticks = newPrice;

    auto event = makeEvent(ClientOrderEventType::Replaced, orderId, state);
    event.qty_lots = state.remaining_qty_lots;
    events_.push(event);

    if (state.remaining_qty_lots == 0) {
        client_orders_.erase(it);
    }
}

void ClientOrderListener::on_replace_reject(const std::uint64_t& orderId,
                                            const char* reason) {
    const auto it = client_orders_.find(orderId);
    if (it == client_orders_.end()) {
        return;
    }
    auto event =
        makeEvent(ClientOrderEventType::ReplaceRejected, orderId, it->second);
    event.reject_reason = mapRejectReason(reason);
    events_.push(event);
}

RejectReason ClientOrderListener::mapRejectReason(const char* reason) noexcept {
    const std::string_view text = reason ? reason : "";
    if (text == "size must be positive") {
        return RejectReason::InvalidQuantity;
    }
    if (text == "not found") {
        return RejectReason::OrderNotFound;
    }
    return RejectReason::InternalError;
}

} // namespace babo::egress
