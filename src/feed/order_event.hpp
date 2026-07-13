#pragma once

#include <cstdint>

namespace babo::feed {

enum class OrderEventType { New, Modify, Cancel, Match };

struct OrderEvent {
    OrderEventType type{};
    std::uint64_t order_id{};
    double price{};
    double size{};
    char side{}; // 'B' or 'S'
};

} // namespace babo::feed
