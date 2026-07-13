#include "core/main_process.hpp"

#include "core/logging.hpp"

#include <chrono>
#include <utility>

namespace babo {

MainProcess::MainProcess()
    : networkThread_([this](std::stop_token st) { networkLoop(st); }),
      engineThread_([this](std::stop_token st) { engineLoop(st); }) {
    spdlog::info("MainProcess constructed — networkThread + engineThread launched");
}

namespace {
// Scale factors mapping Coinbase decimals into the engine's integer domain.
// BTC-USD quotes in $0.01 ticks and trades in 1e-8 (satoshi) base increments.
// The engine now uses uint64 for price/qty/cost, so satoshis fit with headroom
// (21M BTC ~= 2.1e15 sat, well under 2^53 double-exact and 2^64).
constexpr double kPriceScale = 100.0;         // USD -> integer cents
constexpr double kQtyScale = 100'000'000.0;   // BTC -> integer satoshis
} // namespace

std::size_t MainProcess::seedSide(const std::vector<feed::RestingOrder>& orders,
                                  bool is_buy, std::size_t& skipped) {
    std::size_t seeded = 0;
    for (const auto& ro : orders) {
        const double px = ro.price * kPriceScale;
        const double qty = ro.size * kQtyScale;
        // Skip only degenerate dust that rounds to a zero price or quantity.
        if (px < 1.0 || qty < 1.0) {
            ++skipped;
            continue;
        }
        const auto price_ticks = static_cast<std::uint64_t>(px + 0.5);
        const auto qty_lots = static_cast<std::uint64_t>(qty + 0.5);

        simple::SimpleOrder order(is_buy, price_ticks, qty_lots);
        const std::uint32_t engine_id = order.order_id_; // read before the move
        // Direct resting insert — bypasses matching (snapshot orders don't cross).
        if (is_buy) {
            book_.bids().insert(order);
        } else {
            book_.asks().insert(order);
        }
        orderIdMap_.emplace(ro.order_id, engine_id);
        ++seeded;
    }
    return seeded;
}

void MainProcess::reproduceSnapshot(feed::L3Snapshot snapshot) {
    spdlog::info("reproduceSnapshot: seeding book from L3 snapshot seq={} "
                 "({} bids, {} asks)",
                 snapshot.sequence, snapshot.bids.size(), snapshot.asks.size());

    orderIdMap_.clear();
    orderIdMap_.reserve(snapshot.bids.size() + snapshot.asks.size());

    std::size_t skipped = 0;
    const std::size_t seeded_bids = seedSide(snapshot.bids, /*is_buy=*/true, skipped);
    const std::size_t seeded_asks = seedSide(snapshot.asks, /*is_buy=*/false, skipped);

    spdlog::info("reproduceSnapshot: inserted {} bids + {} asks = {} resting "
                 "orders ({} skipped, {} id-mapped)",
                 seeded_bids, seeded_asks, seeded_bids + seeded_asks, skipped,
                 orderIdMap_.size());

    // Verify by reading the top of book straight from the engine's own trees —
    // if these match the snapshot's best bid/ask, the book was built correctly.
    if (auto* bb = book_.bids().get_best()) {
        spdlog::info("  book best bid: ${:.2f} ({} ticks) qty={} across {} orders",
                     static_cast<double>(bb->_price) / kPriceScale, bb->_price,
                     bb->_quantity, bb->_count);
    }
    if (auto* ba = book_.asks().get_best()) {
        spdlog::info("  book best ask: ${:.2f} ({} ticks) qty={} across {} orders",
                     static_cast<double>(ba->_price) / kPriceScale, ba->_price,
                     ba->_quantity, ba->_count);
    }
    spdlog::info("reproduceSnapshot: done");
}

void MainProcess::handleMessage(const FeedMessage& msg) {
    switch (msg.type) {
    case MsgType::New:
        spdlog::info("order NEW    id={} side={} px={} sz={}", msg.order_id,
                     msg.side, msg.price, msg.size);
        // TODO: enqueue into the ingress ring buffer (not implemented yet).
        break;
    case MsgType::Modify:
        spdlog::info("order MODIFY id={} px={} sz={}", msg.order_id, msg.price,
                     msg.size);
        // TODO: enqueue into the ingress ring buffer.
        break;
    case MsgType::Cancel:
        spdlog::info("order CANCEL id={}", msg.order_id);
        // TODO: enqueue into the ingress ring buffer.
        break;
    case MsgType::Match:
        // TODO(correctness-oracle): instead of discarding, compare our engine's
        // own trades against Coinbase `match` events to validate matching
        // correctness (residual divergence = hidden/iceberg liquidity). For now,
        // discard.
        break;
    }
}

void MainProcess::networkLoop(std::stop_token stopToken) {
    spdlog::info("networkThread: started");

    // 1. Fetch the REST L3 snapshot from Coinbase (bootstrap; shells out to
    //    curl inside fetchL3Snapshot). On failure we proceed with an empty
    //    snapshot + logged error rather than blocking the engine thread.
    feed::L3Snapshot snapshot;
    try {
        snapshot = feed::fetchL3Snapshot("BTC-USD");
        spdlog::info("networkThread: fetched L3 snapshot seq={} ({} bids, {} asks)",
                     snapshot.sequence, snapshot.bids.size(),
                     snapshot.asks.size());
    } catch (const std::exception& e) {
        spdlog::error("networkThread: snapshot fetch failed: {}", e.what());
    }

    // 2. Launch snapshot reproduction IMMEDIATELY on its own thread.
    //    std::launch::async (NOT deferred): deferred would only run when the
    //    engine thread calls .get(), executing there instead of concurrently
    //    here — we want it running now while we start reading the feed.
    reproduceSnapshotFuture_ = std::async(
        std::launch::async, [this, snapshot = std::move(snapshot)]() mutable {
            reproduceSnapshot(std::move(snapshot));
        });

    // 3. Publish the future so the engine thread can consume it race-free.
    snapshotFuturePublished_.count_down();

    // 4. Idle until shutdown. The real WebSocket read/parse loop lands here:
    //    each parsed `full`-channel event -> handleMessage() -> ingress ring.
    while (!stopToken.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    spdlog::info("networkThread: stop requested, exiting");
}

void MainProcess::engineLoop(std::stop_token stopToken) {
    spdlog::info("engineThread: started, waiting for snapshot future");

    // Wait until the network thread has created the reproduce-snapshot future,
    // then block until the snapshot has been fully reproduced.
    snapshotFuturePublished_.wait();
    reproduceSnapshotFuture_.get();
    spdlog::info("engineThread: snapshot reproduced — consuming ingress buffer");

    // Consume the (not-yet-implemented) ingress ring buffer. For now: idle in a
    // 5s loop until shutdown.
    while (!stopToken.stop_requested()) {
        // TODO: drain the ingress ring and feed babo_matching_engine.
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    spdlog::info("engineThread: stop requested, exiting");
}

} // namespace babo
