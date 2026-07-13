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

void MainProcess::reproduceSnapshot(SnapshotData /*snapshot*/) {
    spdlog::info("reproduceSnapshot: rebuilding book from L3 snapshot (stub)");
    // STUB: later, insert each resting [price, size, order_id] order from the
    // snapshot into the book so it is seeded before live events are applied.
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // simulate work
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

    // 1. Receive the REST L3 snapshot. STUB for now — later this is
    //    GET https://api.exchange.coinbase.com/products/BTC-USD/book?level=3.
    SnapshotData snapshot{};
    spdlog::info("networkThread: received L3 snapshot (stub)");

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

    // 4. Start queuing (for now: logging) incoming order messages. These sample
    //    messages STAND IN for parsed Coinbase `full`-channel events — replace
    //    with the real WebSocket read/parse loop later.
    const FeedMessage sample[] = {
        {MsgType::New, 1001, 50000.0, 0.5, 'B'},
        {MsgType::New, 1002, 50010.0, 0.3, 'S'},
        {MsgType::Modify, 1001, 50000.0, 0.4, 'B'},
        {MsgType::Match, 1002, 0.0, 0.0, 'S'}, // discarded (our engine matches)
        {MsgType::Cancel, 1001, 0.0, 0.0, 'B'},
    };
    for (const auto& msg : sample) {
        handleMessage(msg);
    }

    // Idle until shutdown — the real WS message read loop lands here.
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
