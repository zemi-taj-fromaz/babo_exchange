#pragma once

#include "book/matching_book.h"
#include "core/ingress_event.hpp"
#include "egress/client_order_listener.hpp"
#include "egress/latest_depth_mailbox.hpp"
#include "feed/bitstamp.hpp"
#include "feed/order_event.hpp"

#include <rigtorp/MPMCQueue.h>
#include <rigtorp/SPSCQueue.h>
#include <rigtorp/MPMCQueue.h>

#include <cstddef>
#include <cstdint>
#include <future>
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
// Ingress is a bounded Rigtorp-style MPMC queue. We currently have one producer
// (the websocket callback), and the order gateway can become a second producer
// without changing the engine thread's single-consumer ownership of book_.
class MainProcess {
public:
    MainProcess();
    ~MainProcess();

    MainProcess(const MainProcess&) = delete;
    MainProcess& operator=(const MainProcess&) = delete;
    MainProcess(MainProcess&&) = delete;
    MainProcess& operator=(MainProcess&&) = delete;

private:
    void networkLoop(std::stop_token stopToken);
    void engineLoop(std::stop_token stopToken);
    void gatewayLoop(std::stop_token stopToken);

    // Engine-thread only: rebuild the book from the L3 snapshot before draining
    // the live events that accumulated in ingress_ while REST was in flight.
    void reproduceSnapshot(const feed::L3Snapshot& snapshot);

    // Insert one side of the snapshot into the book as resting orders (direct
    // tree insert, not add(), so no matching runs; these orders are already
    // resting). Returns how many were seeded; bumps skipped for degenerate
    // zero-price/zero-quantity rows.
    std::size_t seedSide(const std::vector<feed::RestingOrder>& orders,
                         bool is_buy, std::size_t& skipped);

    // Producer side: called by the websocket callback thread. It blocks only if
    // the engine falls behind the fixed-size SPSC queue.
    void enqueueFeedEvent(const feed::OrderEvent& event);
    bool tryEnqueueClientEvent(const core::IngressEvent& event);

    // Consumer side: engine thread only. Applies one normalized feed event to
    // the local book reconstruction.
    void applyIngressEvent(const core::IngressEvent& event);
    void applyFeedEvent(const core::IngressEvent& event);
    void applyClientEvent(const core::IngressEvent& event);

    // Consumer side: engine thread only. Applies one normalized ingress command
    // to the local book reconstruction.
    void applyIngressEvent(const core::IngressEvent& event);

    // Engine-thread only: copy the current top-five book into the latest-value
    // mailbox. The gateway may skip intermediate versions but never reads book_.
    void publishDepthSnapshot();

    // The network thread publishes the fetched snapshot; the engine thread
    // owns both snapshot reproduction and every subsequent book mutation.
    std::promise<feed::L3Snapshot> snapshotPromise_;
    std::future<feed::L3Snapshot> snapshotFuture_;

    // SPSC egress from the engine thread to the gateway thread. The
    // engine-thread order listener is the sole producer; gatewayLoop is the sole
    // consumer that will route these private reports back to client sessions.
    egress::ClientOrderEventQueue clientEgress_;
    egress::ClientOrderListener clientOrderListener_;
    egress::LatestDepthMailbox depthMailbox_;

    // The matching core. After snapshot reproduction completes, only the engine
    // thread mutates it by draining ingress_.
    book::matching_book<> book_;

    // One live-feed producer, one engine-thread consumer. Capacity is in events;
    // Rigtorp internally reserves one slack slot.
    rigtorp::MPMCQueue<core::IngressEvent> ingress_;

    // Started in the constructor body, after the listener is registered.
    // Declaration order gives reverse shutdown: network -> engine -> gateway,
    // so consumers remain alive while their producers finish.
    std::jthread gatewayThread_;
    std::jthread engineThread_;
    std::jthread networkThread_;
};

} // namespace babo
