#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace babo::egress {

inline constexpr std::size_t kPublishedDepthLevels = 5;

struct DepthLevelSnapshot {
    std::uint64_t price_ticks{};
    std::uint64_t qty_lots{};
    std::uint32_t order_count{};
};

struct DepthSnapshot {
    std::uint64_t sequence{};
    std::array<DepthLevelSnapshot, kPublishedDepthLevels> bids{};
    std::array<DepthLevelSnapshot, kPublishedDepthLevels> asks{};
};

static_assert(std::is_trivially_copyable_v<DepthSnapshot>);

} // namespace babo::egress
