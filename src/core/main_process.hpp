#pragma once

#include <cstdint>
#include <future>
#include <latch>
#include <stop_token>
#include <thread>

namespace babo {

// Placeholder for the REST L3 snapshot payload. Later: the parsed
// [price, size, order_id] resting orders from
// GET .../products/BTC-USD/book?level=3, used to seed the book before live
// events are consumed.
struct SnapshotData {
    // stub — no fields yet
};

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
    void reproduceSnapshot(SnapshotData snapshot);

    // Route one inbound message: New/Modify/Cancel are logged (later: enqueued
    // into the ingress ring); Match is discarded (see TODO in the .cpp).
    void handleMessage(const FeedMessage& msg);

    // Snapshot-reproduction future: created in the network thread, consumed
    // (get()) in the engine thread.
    std::future<void> reproduceSnapshotFuture_;
    // Signalled by the network thread once reproduceSnapshotFuture_ has been
    // assigned, so the engine thread can take it without a data race.
    std::latch snapshotFuturePublished_{1};

    // Declared last so the latch/future above are fully constructed before the
    // threads that use them start running.
    std::jthread networkThread_;
    std::jthread engineThread_;
};

} // namespace babo
