#pragma once

#include "core/order_identity.hpp"

#include <cstdint>
#include <type_traits>

namespace babo::egress {

enum class ClientOrderEventType : std::uint8_t {
    Accepted,
    Rejected,
    Cancelled,
    CancelRejected,
    Replaced,
    ReplaceRejected,
    Fill,
};

enum class FillRole : std::uint8_t { None, Maker, Taker };

enum class RejectReason : std::uint8_t {
    None,
    InvalidQuantity,
    OrderNotFound,
    DuplicateOrder,
    NotOwner,
    InternalError,
};

// One private, ordered notification for exactly one client session. Unused
// fields remain zero so the type stays fixed-size and allocation-free in SPSC.
struct ClientOrderEvent {
    ClientOrderEventType type{};
    std::uint64_t event_sequence{};
    core::SessionId target_session_id{};
    core::ClientOrderId client_order_id{};
    core::ExchangeOrderId exchange_order_id{};
    std::uint64_t price_ticks{};
    std::uint64_t qty_lots{};
    std::uint64_t remaining_qty_lots{};
    FillRole fill_role{FillRole::None};
    RejectReason reject_reason{RejectReason::None};
};

static_assert(std::is_trivially_copyable_v<ClientOrderEvent>);

} // namespace babo::egress
