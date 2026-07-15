#pragma once

#include <cstdint>

namespace babo::feed {

enum class OrderEventType : std::uint8_t { New, Modify, Cancel, Match };
enum class OrderSource : std::uint8_t { OrderBook, StopOrder };

struct OrderEvent {
    OrderEventType type{};
    OrderSource source{OrderSource::OrderBook};
    std::uint8_t order_subtype{};
    std::uint64_t microtimestamp{};
    std::uint64_t order_id{};
    std::uint64_t price_ticks{};
    std::uint64_t qty_lots{};
    std::uint64_t original_qty_lots{};
    std::uint64_t traded_qty_lots{};
    char side{}; // 'B' or 'S'
};

} // namespace babo::feed
