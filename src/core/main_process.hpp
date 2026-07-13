#pragma once

#include "book/matching_book.h"
#include "feed/coinbase.hpp"

#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace babo {

// A normalized inbound feed message. Later this is what the Coinbase
// LiveSocketSource produces (received -> New, change -> Modify, done/canceled ->
// Cancel) and what gets pushed into the ingress ring buffer. `Match` is
// Coinbase's own trade result — we discard it for book-building because our
// engine does its own matching.
enum class MsgType { New, Modify, Cancel, Match };

struct FeedMessage {
    MsgType type{};
    std::uint64_t order_id{};
    double price{};
    double size{};
    char side{}; // 'B' or 'S'
};

// Owns the exchange core process's long-lived threads:
//   - networkThread: receives the L3 snapshot, kicks off snapshot reproduction
//     asynchronously, then logs (later: enqueues) incoming order flow.
//   - engineThread:  waits on the snapshot reproduction, then consumes the
//     ingress buffer and drives the matching engine.
// Both are std::jthreads: construction launches them; destruction auto-requests
// stop + joins.
class MainProcess {
public:
    MainProcess();
    ~MainProcess() = default; // jthreads auto stop + join

    MainProcess(const MainProcess&) = delete;
    MainProcess& operator=(const MainProcess&) = delete;
    MainProcess(MainProcess&&) = delete;
    MainProcess& operator=(MainProcess&&) = delete;

private:
    void networkLoop(std::stop_token stopToken);
    void engineLoop(std::stop_token stopToken);

    // Runs asynchronously (immediate std::launch::async) off the network thread:
    // rebuilds the book from the L3 snapshot. The engine thread blocks on the
    // resulting future before it starts consuming live order flow.
    void reproduceSnapshot(feed::L3Snapshot snapshot);

    // Insert one side of the snapshot into the book as resting orders (direct
    // tree insert — NOT add(), so no matching runs; these orders are already
    // resting). Returns how many were seeded; bumps `skipped` for orders that
    // can't be represented in the engine's uint32 price/qty domain.
    std::size_t seedSide(const std::vector<feed::RestingOrder>& orders,
                         bool is_buy, std::size_t& skipped);

    // Route one inbound message: New/Modify/Cancel are logged (later: enqueued
    // into the ingress ring); Match is discarded (see TODO in the .cpp).
    void handleMessage(const FeedMessage& msg);

    // Snapshot-reproduction future: created in the network thread, consumed
    // (get()) in the engine thread.
    std::future<void> reproduceSnapshotFuture_;
    // Signalled by the network thread once reproduceSnapshotFuture_ has been
    // assigned, so the engine thread can take it without a data race.
    std::latch snapshotFuturePublished_{1};

    // The matching core. Single-writer: only the engine thread touches it (the
    // snapshot seed + the drained order flow), keeping it lock-free and
    // deterministic. Default SIZE=5 depth levels, TRADE_CAP=256 trade ring.
    book::matching_book<> book_;

    // Coinbase order UUID (128-bit) -> engine order id (assigned at seed/insert
    // time). Lets later live done/change/match events, which reference the UUID,
    // locate the resting order in book_. Written during snapshot reproduction;
    // read on the engine thread when applying live flow.
    std::unordered_map<feed::Uuid, std::uint32_t> orderIdMap_;

    // Declared last so the latch/future above are fully constructed before the
    // threads that use them start running.
    std::jthread networkThread_;
    std::jthread engineThread_;
};

} // namespace babo
