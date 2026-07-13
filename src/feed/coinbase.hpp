#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace babo::feed {

// One resting order from the L3 snapshot: [price, size, order_id].
struct RestingOrder {
    double price{};
    double size{};
    std::string order_id; // Coinbase order UUID
    char side{};          // 'B' (bid) or 'S' (ask)
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
