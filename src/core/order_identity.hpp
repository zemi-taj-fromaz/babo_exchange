#pragma once

#include <cstdint>

namespace babo::core {

using ExchangeOrderId = std::uint64_t;
using ClientOrderId = std::uint64_t;
using SessionId = std::uint64_t;

// Bitstamp order ids are kept verbatim in the lower half of the uint64 space.
// Locally generated client orders reserve the high bit, giving both sources a
// collision-free id namespace while the matching core continues using uint64.
inline constexpr ExchangeOrderId kClientOrderIdBit = ExchangeOrderId{1} << 63;
inline constexpr ExchangeOrderId kClientOrderSequenceMask =
    kClientOrderIdBit - 1;

[[nodiscard]] constexpr bool isClientOrderId(ExchangeOrderId id) noexcept {
    return (id & kClientOrderIdBit) != 0;
}

[[nodiscard]] constexpr bool isBitstampOrderId(ExchangeOrderId id) noexcept {
    return id != 0 && !isClientOrderId(id);
}

[[nodiscard]] constexpr ExchangeOrderId makeClientOrderId(
    ExchangeOrderId sequence) noexcept {
    return kClientOrderIdBit | (sequence & kClientOrderSequenceMask);
}

} // namespace babo::core
