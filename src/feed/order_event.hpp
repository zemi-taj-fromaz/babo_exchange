#pragma once

#include <cstdint>

namespace babo::feed {

enum class OrderEventType { New, Modify, Cancel, Match };

struct OrderEvent {
    OrderEventType type{};
    std::uint64_t order_id{};
    std::uint64_t price_ticks{};
    std::uint64_t qty_lots{};
    char side{}; // 'B' or 'S'
};

} // namespace babo::feed
