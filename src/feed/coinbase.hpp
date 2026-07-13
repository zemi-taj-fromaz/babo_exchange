#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace babo::feed {

// A 128-bit order identifier (a Coinbase order UUID), stored as two 64-bit
// halves. It's a trivially-copyable 16-byte POD: no heap, a two-word compare,
// and a cheap hash — far leaner than keying maps on a std::string. (Portable
// stand-in for a 128-bit int, which isn't available on MSVC.)
struct Uuid {
    std::uint64_t hi{};
    std::uint64_t lo{};
    friend bool operator==(const Uuid&, const Uuid&) = default;
};

// Parse the canonical Coinbase UUID text ("7b3741ab-8b6a-4c9a-8a53-a7bf8d0627d4")
// into a Uuid: dashes ignored, expects exactly 32 hex digits. Throws
// std::invalid_argument on malformed input.
Uuid parseUuid(std::string_view s);

// One resting order from the L3 snapshot: [price, size, order_id].
struct RestingOrder {
    double price{};
    double size{};
    Uuid order_id;  // Coinbase order UUID, as 128 bits
    char side{};    // 'B' (bid) or 'S' (ask)
};

// Parsed Coinbase Exchange REST L3 order-book snapshot. `sequence` is the sync
// point S used to reconcile against the live WS feed (discard buffered messages
// with sequence <= S; a later gap forces a re-snapshot).
struct L3Snapshot {
    std::uint64_t sequence{};
    std::vector<RestingOrder> bids;
    std::vector<RestingOrder> asks;
};

// Fetch the L3 snapshot for `product` (e.g. "BTC-USD"):
//   GET https://api.exchange.coinbase.com/products/{product}/book?level=3
//
// TRANSPORT SEAM: currently shells out to the system `curl`. The snapshot is a
// one-time bootstrap, NOT the latency-critical path, so a subprocess is fine
// here — and it keeps us off a fragile in-process TLS dependency for now. Swap
// for an in-process TLS HTTP client later without touching callers.
//
// Throws std::runtime_error on transport/HTTP failure and on malformed JSON.
L3Snapshot fetchL3Snapshot(const std::string& product);

} // namespace babo::feed

// Hash so Uuid can key an unordered_map/set (boost-style combine of the halves).
template <>
struct std::hash<babo::feed::Uuid> {
    std::size_t operator()(const babo::feed::Uuid& u) const noexcept {
        std::uint64_t h = u.hi;
        h ^= u.lo + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};
