#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace babo::feed {

// One resting order from the L3 snapshot: [price, amount, order_id].
struct RestingOrder {
    double price{};
    double size{};
    std::uint64_t order_id{}; // Bitstamp order id (integer)
    char side{};              // 'B' (bid) or 'S' (ask)
};

// Parsed Bitstamp REST L3 order-book snapshot. `microtimestamp` is the venue's
// as-of stamp (Bitstamp has no per-message sequence like Coinbase).
struct L3Snapshot {
    std::uint64_t microtimestamp{};
    std::vector<RestingOrder> bids;
    std::vector<RestingOrder> asks;
};

// Fetch the L3 snapshot for `pair` (e.g. "btcusd"):
//   GET https://www.bitstamp.net/api/v2/order_book/{pair}/?group=2
// group=2 = non-aggregated, each order with its id (matches the live_orders feed).
//
// TRANSPORT SEAM: shells out to the system `curl` (bootstrap, not the hot path).
// Throws std::runtime_error on transport/HTTP failure and on malformed JSON.
L3Snapshot fetchL3Snapshot(const std::string& pair);

} // namespace babo::feed
