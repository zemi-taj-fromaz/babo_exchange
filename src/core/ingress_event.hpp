#pragma once

#include "core/order_identity.hpp"

#include <cstdint>
#include <type_traits>

namespace babo::core {

enum class IngressSource : std::uint8_t { Feed, Client };
enum class IngressEventType : std::uint8_t { New, Modify, Cancel };

struct IngressEvent {
    IngressSource source{IngressSource::Feed};
    IngressEventType type{};
    SessionId session_id{};
    ClientOrderId client_order_id{};
    ExchangeOrderId order_id{};
    char side{}; // 'B' or 'S'
    std::uint64_t price_ticks{};
    std::uint64_t qty_lots{};
};

static_assert(std::is_trivially_copyable_v<IngressEvent>);

} // namespace babo::core
