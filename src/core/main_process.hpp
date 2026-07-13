#pragma once

#include "book/matching_book.h"
#include "feed/bitstamp.hpp"
#include "feed/order_event.hpp"

#include <rigtorp/SPSCQueue.h>

#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <stop_token>
#include <thread>
#include <vector>

namespace babo {

// Owns the exchange core process's long-lived threads:
//   - networkThread: receives the L3 snapshot, kicks off snapshot reproduction,
//     then runs live feed/order-flow ingress.
//   - engineThread: waits on snapshot reproduction, then consumes the ingress
//     queue and drives the matching book.
//
// Current ingress is Rigtorp SPSCQueue: one websocket callback producer and one
// engine consumer. When the order gateway becomes a second producer, this seam
// is where the planned MPSC ring replaces it.
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
    // tree insert, not add(), so no matching runs; these orders are already
    // resting). Returns how many were seeded; bumps skipped for degenerate
    // zero-price/zero-quantity rows.
    std::size_t seedSide(const std::vector<feed::RestingOrder>& orders,
                         bool is_buy, std::size_t& skipped);

    // Producer side: called by the websocket callback thread. It blocks only if
    // the engine falls behind the fixed-size SPSC queue.
    void enqueueOrderEvent(const feed::OrderEvent& event);

    // Consumer side: engine thread only. Applies one normalized feed event to
    // the local book reconstruction.
    void applyOrderEvent(const feed::OrderEvent& event);

    // Consumer side: render top-of-book depth from the same engine thread that
    // owns the book, avoiding cross-thread reads.
    void renderDepth(std::uint64_t appliedEvents);

    // Snapshot-reproduction future: created in the network thread, consumed
    // (get()) in the engine thread.
    std::future<void> reproduceSnapshotFuture_;
    // Signalled by the network thread once reproduceSnapshotFuture_ has been
    // assigned, so the engine thread can take it without a data race.
    std::latch snapshotFuturePublished_{1};

    // The matching core. After snapshot reproduction completes, only the engine
    // thread mutates it by draining ingress_.
    book::matching_book<> book_;

    // One live-feed producer, one engine-thread consumer. Capacity is in events;
    // Rigtorp internally reserves one slack slot.
    rigtorp::SPSCQueue<feed::OrderEvent> ingress_;

    // Declared last so the latch/future/queue above are fully constructed before
    // the threads that use them start running.
    std::jthread networkThread_;
    std::jthread engineThread_;
};

} // namespace babo
