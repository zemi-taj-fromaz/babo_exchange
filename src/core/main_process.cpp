#include "core/main_process.hpp"

#include "core/logging.hpp"
#include "core/order_identity.hpp"
#include "feed/bitstamp_ws.hpp"
#include "gateway/tcp_gateway.hpp"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace babo {

MainProcess::MainProcess()
    : snapshotFuture_(snapshotPromise_.get_future()), clientEgress_(1u << 14),
      clientOrderListener_(clientEgress_), ingress_(1u << 16) {
    book_.set_order_listener(&clientOrderListener_);
    gatewayThread_ =
        std::jthread([this](std::stop_token st) { gatewayLoop(st); });
    engineThread_ =
        std::jthread([this](std::stop_token st) { engineLoop(st); });
    networkThread_ =
        std::jthread([this](std::stop_token st) { networkLoop(st); });
    spdlog::info("MainProcess constructed - network + engine + gateway threads launched");
}

MainProcess::~MainProcess() {
    // Stop both producers first, while the engine consumer is still draining.
    networkThread_.request_stop();
    gatewayThread_.request_stop();
    if (networkThread_.joinable()) networkThread_.join();
    if (gatewayThread_.joinable()) gatewayThread_.join();
    engineThread_.request_stop();
    if (engineThread_.joinable()) engineThread_.join();
}

namespace {
constexpr double kPriceScale = 100.0;         // USD -> integer cents
} // namespace

std::size_t MainProcess::seedSide(const std::vector<feed::RestingOrder>& orders,
                                  bool is_buy, std::size_t& skipped) {
    std::size_t seeded = 0;
    for (const auto& ro : orders) {
        if (!core::isBitstampOrderId(ro.order_id)) {
            spdlog::warn("snapshot: Bitstamp id uses reserved client namespace: {}",
                         ro.order_id);
            ++skipped;
            continue;
        }
        const auto price_ticks = ro.price_ticks;
        const auto qty_lots = ro.qty_lots;
        if (price_ticks == 0 || qty_lots == 0) {
            ++skipped;
            continue;
        }

        simple::SimpleOrder order(is_buy, price_ticks, qty_lots);
        order.order_id_ = ro.order_id;
        if (is_buy) {
            book_.bids().insert(order);
        } else {
            book_.asks().insert(order);
        }
        ++seeded;
    }
    return seeded;
}

void MainProcess::reproduceSnapshot(const feed::L3Snapshot& snapshot) {
    spdlog::info("reproduceSnapshot: seeding book from L3 snapshot "
                 "microtimestamp={} ({} bids, {} asks)",
                 snapshot.microtimestamp, snapshot.bids.size(),
                 snapshot.asks.size());

    std::size_t skipped = 0;
    const std::size_t seeded_bids = seedSide(snapshot.bids, true, skipped);
    const std::size_t seeded_asks = seedSide(snapshot.asks, false, skipped);

    spdlog::info("reproduceSnapshot: inserted {} bids + {} asks = {} resting "
                 "orders ({} skipped)",
                 seeded_bids, seeded_asks, seeded_bids + seeded_asks, skipped);

    auto* best_bid = book_.bids().get_best();
    auto* best_ask = book_.asks().get_best();

    if (best_bid) {
        spdlog::info("  book best bid: ${:.2f} ({} ticks) qty={} across {} orders",
                     static_cast<double>(best_bid->_price) / kPriceScale,
                     best_bid->_price, best_bid->_quantity, best_bid->_count);
    }
    if (best_ask) {
        spdlog::info("  book best ask: ${:.2f} ({} ticks) qty={} across {} orders",
                     static_cast<double>(best_ask->_price) / kPriceScale,
                     best_ask->_price, best_ask->_quantity, best_ask->_count);
    }

    if (auto sync_log = spdlog::get("babo_sync")) {
        sync_log->info(
            "SYNC complete microtimestamp={} seeded={} skipped={} "
            "best_bid=${:.2f} bid_qty={} bid_orders={} "
            "best_ask=${:.2f} ask_qty={} ask_orders={}",
            snapshot.microtimestamp, seeded_bids + seeded_asks, skipped,
            best_bid ? static_cast<double>(best_bid->_price) / kPriceScale : 0.0,
            best_bid ? best_bid->_quantity : 0,
            best_bid ? best_bid->_count : 0,
            best_ask ? static_cast<double>(best_ask->_price) / kPriceScale : 0.0,
            best_ask ? best_ask->_quantity : 0,
            best_ask ? best_ask->_count : 0);
    }
    spdlog::info("reproduceSnapshot: done");
}

void MainProcess::enqueueFeedEvent(const feed::OrderEvent& event) {
    core::IngressEvent ingressEvent;
    ingressEvent.source = core::IngressSource::Feed;
    ingressEvent.order_id = event.order_id;
    ingressEvent.price_ticks = event.price_ticks;
    ingressEvent.qty_lots = event.qty_lots;
    ingressEvent.original_qty_lots = event.original_qty_lots;
    ingressEvent.traded_qty_lots = event.traded_qty_lots;
    ingressEvent.order_subtype = event.order_subtype;
    ingressEvent.active_orderbook_order =
        event.source == feed::OrderSource::OrderBook;
    ingressEvent.side = event.side;
    switch (event.type) {
    case feed::OrderEventType::New: ingressEvent.type = core::IngressEventType::New; break;
    case feed::OrderEventType::Modify: ingressEvent.type = core::IngressEventType::Modify; break;
    case feed::OrderEventType::Cancel: ingressEvent.type = core::IngressEventType::Cancel; break;
    case feed::OrderEventType::Match: return;
    }
    ingress_.push(ingressEvent);
}

bool MainProcess::tryEnqueueClientEvent(const core::IngressEvent& event) {
    return ingress_.try_push(event);
}

void MainProcess::applyIngressEvent(const core::IngressEvent& event) {
    if (event.source == core::IngressSource::Client) {
        applyClientEvent(event);
    } else {
        applyFeedEvent(event);
    }
}

void MainProcess::applyFeedEvent(const core::IngressEvent& event) {
    if (!core::isBitstampOrderId(event.order_id)) {
        spdlog::warn("engine: Bitstamp id uses reserved client namespace: {}",
                     event.order_id);
        return;
    }
    const auto price_ticks = event.price_ticks;
    const auto qty_lots = event.qty_lots;

    // Stop-order lifecycle events are not active visible-book orders. Bitstamp
    // emits a separate orderbook-source event when an order becomes active.
    if (!event.active_orderbook_order) {
        return;
    }

    switch (event.type) {
    case core::IngressEventType::New: {
        const auto original_qty =
            event.original_qty_lots != 0 ? event.original_qty_lots : qty_lots;
        if (original_qty == 0 || (event.side != 'B' && event.side != 'S')) {
            return;
        }
        if (book_.bids().find_order(event.order_id) ||
            book_.asks().find_order(event.order_id)) {
            return;
        }

        // Bitstamp explicitly identifies subtype 2 as MARKET. Other aggressive
        // subtypes still carry a protection price and must not cross beyond it.
        const bool is_market = event.order_subtype == 2;
        if (!is_market && price_ticks == 0) {
            return;
        }

        // Subtype 5 is maker-or-cancel (post-only). It must never remove
        // liquidity. If it crosses our current opposite best, Bitstamp would
        // cancel it rather than execute it.
        if (event.order_subtype == 5) {
            const auto* oppositeBest = event.side == 'B'
                                           ? book_.asks().get_best()
                                           : book_.bids().get_best();
            const bool crosses =
                oppositeBest != nullptr &&
                (event.side == 'B' ? price_ticks >= oppositeBest->_price
                                   : price_ticks <= oppositeBest->_price);
            if (crosses) {
                return;
            }
        }

        book::OrderConditions conditions = book::OrderCondition::oc_no_conditions;
        if (event.order_subtype == 4) {
            conditions = book::OrderCondition::oc_immediate_or_cancel;
        } else if (event.order_subtype == 6) {
            conditions = book::OrderCondition::oc_fill_or_kill;
        }

        simple::SimpleOrder order(event.side == 'B',
                                  is_market ? book::MARKET_ORDER_PRICE
                                            : price_ticks,
                                  original_qty, 0, conditions);
        order.order_id_ = event.order_id;
        feedTradedQty_[event.order_id] = event.traded_qty_lots;
        book_.add(order);
        break;
    }
    case core::IngressEventType::Modify: {
        simple::SimpleOrder* existing = book_.bids().find_order(event.order_id);
        if (!existing) {
            existing = book_.asks().find_order(event.order_id);
        }
        if (!existing) {
            // An aggressive order may already be gone because our matcher
            // applied its fill before Bitstamp's lifecycle confirmation arrived.
            feedTradedQty_[event.order_id] = event.traded_qty_lots;
            return;
        }

        const auto tradedIt = feedTradedQty_.find(event.order_id);
        const auto previousTraded = tradedIt == feedTradedQty_.end()
                                        ? std::uint64_t{0}
                                        : tradedIt->second;
        const bool fillConfirmation =
            event.traded_qty_lots > 0 &&
            price_ticks == existing->price() &&
            qty_lots <= existing->open_qty() &&
            (event.traded_qty_lots > previousTraded ||
             qty_lots < existing->open_qty());
        feedTradedQty_[event.order_id] = event.traded_qty_lots;
        if (fillConfirmation) {
            return;
        }
        if (qty_lots == 0) {
            book_.cancel(event.order_id);
            return;
        }

        const auto new_price =
            (price_ticks == existing->price()) ? book::PRICE_UNCHANGED : price_ticks;
        if (new_price == book::PRICE_UNCHANGED &&
            qty_lots == existing->open_qty()) {
            return;
        }
        const auto size_delta =
            static_cast<std::int64_t>(qty_lots) -
            static_cast<std::int64_t>(existing->open_qty());
        book_.replace(event.order_id, size_delta, new_price);
        break;
    }
    case core::IngressEventType::Cancel: {
        const bool fillConfirmation =
            event.traded_qty_lots > 0 &&
            (event.qty_lots == 0 ||
             (event.original_qty_lots != 0 &&
              event.traded_qty_lots >= event.original_qty_lots));
        feedTradedQty_.erase(event.order_id);
        if (fillConfirmation) {
            return;
        }
        if (book_.bids().find_order(event.order_id) ||
            book_.asks().find_order(event.order_id)) {
            book_.cancel(event.order_id);
        }
        break;
    }
    }
}

void MainProcess::applyClientEvent(const core::IngressEvent& event) {
    if (event.type == core::IngressEventType::New) {
        if (!core::isClientOrderId(event.order_id) ||
            event.session_id == 0 || event.price_ticks == 0 ||
            event.qty_lots == 0 || (event.side != 'B' && event.side != 'S')) {
            return;
        }
        if (!clientOrderListener_.trackClientOrder(
                event.order_id, event.session_id, event.client_order_id,
                event.price_ticks, event.qty_lots)) {
            return;
        }
        simple::SimpleOrder order(event.side == 'B', event.price_ticks,
                                  event.qty_lots);
        order.order_id_ = event.order_id;
        book_.add(order);
        return;
    }
    if (event.type == core::IngressEventType::Cancel) {
        if (!core::isClientOrderId(event.order_id)) {
            clientOrderListener_.emitCancelRejected(
                event.session_id, event.order_id,
                egress::RejectReason::NotOwner);
            return;
        }
        if (!clientOrderListener_.ownsClientOrder(event.order_id,
                                                   event.session_id)) {
            clientOrderListener_.emitCancelRejected(
                event.session_id, event.order_id,
                egress::RejectReason::NotOwner);
            return;
        }
        book_.cancel(event.order_id);
    }
}

void MainProcess::publishDepthSnapshot() {
    auto& depth = book_.depth();
    const auto* bids = depth.bids();
    const auto* asks = depth.asks();

    egress::DepthSnapshot snapshot;
    for (std::size_t i = 0; i < egress::kPublishedDepthLevels; ++i) {
        snapshot.bids[i] = egress::DepthLevelSnapshot{
            bids[i].price(), bids[i].aggregate_qty(), bids[i].order_count()};
        snapshot.asks[i] = egress::DepthLevelSnapshot{
            asks[i].price(), asks[i].aggregate_qty(), asks[i].order_count()};
    }
    depthMailbox_.publish(snapshot);
}

void MainProcess::networkLoop(std::stop_token stopToken) {
    spdlog::info("networkThread: started");

    bool snapshotPublished = false;
    try {
        std::vector<feed::OrderEvent> bufferedEvents;
        bufferedEvents.reserve(1u << 14);
        std::mutex bufferedEventsMutex;
        bool buffering = true;

        auto subscriptionPromise = std::make_shared<std::promise<void>>();
        auto subscriptionFuture = subscriptionPromise->get_future();
        auto subscriptionSignaled = std::make_shared<std::atomic_bool>(false);

        feed::BitstampFeed liveFeed("btcusd");
        liveFeed.setOrderHandler(
            [this, &bufferedEvents, &bufferedEventsMutex,
             &buffering](const feed::OrderEvent& event) {
                std::unique_lock lock(bufferedEventsMutex);
                if (buffering) {
                    bufferedEvents.push_back(event);
                    return;
                }
                lock.unlock();
                enqueueFeedEvent(event);
            });
        liveFeed.setSubscriptionHandler(
            [subscriptionPromise = std::move(subscriptionPromise),
             subscriptionSignaled]() mutable {
                if (!subscriptionSignaled->exchange(true)) {
                    subscriptionPromise->set_value();
                }
            });
        liveFeed.start();

        const auto subscriptionDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!stopToken.stop_requested()) {
            if (subscriptionFuture.wait_for(std::chrono::milliseconds(50)) ==
                std::future_status::ready) {
                break;
            }
            if (std::chrono::steady_clock::now() >= subscriptionDeadline) {
                throw std::runtime_error(
                    "timed out waiting for Bitstamp subscription confirmation");
            }
        }
        if (stopToken.stop_requested()) {
            liveFeed.stop();
            return;
        }

        // Establish overlap between the REST snapshot and the buffered stream.
        // Bitstamp may return a slightly cached snapshot whose timestamp
        // predates the subscription, which would leave an unrecoverable gap.
        std::uint64_t firstBufferedMicrotimestamp = 0;
        const auto firstEventDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!stopToken.stop_requested() &&
               std::chrono::steady_clock::now() < firstEventDeadline) {
            {
                std::scoped_lock lock(bufferedEventsMutex);
                if (!bufferedEvents.empty()) {
                    firstBufferedMicrotimestamp =
                        bufferedEvents.front().microtimestamp;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (stopToken.stop_requested()) {
            liveFeed.stop();
            return;
        }

        // The network thread fetches data; the engine thread exclusively owns
        // reconstruction and every subsequent mutation of the matching book.
        feed::L3Snapshot snapshot;
        std::size_t snapshotAttempts = 0;
        const auto overlapDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            snapshot = feed::fetchL3Snapshot("btcusd");
            ++snapshotAttempts;
            if (firstBufferedMicrotimestamp == 0 ||
                snapshot.microtimestamp >= firstBufferedMicrotimestamp) {
                break;
            }
            if (std::chrono::steady_clock::now() >= overlapDeadline) {
                throw std::runtime_error(
                    "Bitstamp snapshot never overlapped buffered live stream");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while (!stopToken.stop_requested());
        if (stopToken.stop_requested()) {
            liveFeed.stop();
            return;
        }

        const auto snapshotMicrotimestamp = snapshot.microtimestamp;
        spdlog::info("networkThread: fetched L3 snapshot microtimestamp={} "
                     "({} bids, {} asks, attempts={}, first_live={})",
                     snapshot.microtimestamp, snapshot.bids.size(),
                     snapshot.asks.size(), snapshotAttempts,
                     firstBufferedMicrotimestamp);
        snapshotPromise_.set_value(std::move(snapshot));
        snapshotPublished = true;

        std::size_t replayed = 0;
        std::size_t skipped = 0;
        {
            std::scoped_lock lock(bufferedEventsMutex);
            for (const auto& event : bufferedEvents) {
                if (event.microtimestamp != 0 && snapshotMicrotimestamp != 0 &&
                    event.microtimestamp <= snapshotMicrotimestamp) {
                    ++skipped;
                    continue;
                }
                enqueueFeedEvent(event);
                ++replayed;
            }
            buffering = false;
        }
        spdlog::info("networkThread: replayed {} buffered live events "
                     "after snapshot ({} skipped as already covered)",
                     replayed, skipped);

        while (!stopToken.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        liveFeed.stop();
    } catch (const std::exception& e) {
        if (!snapshotPublished) {
            snapshotPromise_.set_exception(std::current_exception());
        }
        spdlog::error("networkThread: initial synchronization failed: {}", e.what());
    } catch (...) {
        if (!snapshotPublished) {
            snapshotPromise_.set_exception(std::current_exception());
        }
        spdlog::error("networkThread: initial synchronization failed");
    }
    spdlog::info("networkThread: stop requested, exiting");
}

void MainProcess::engineLoop(std::stop_token stopToken) {
    spdlog::info("engineThread: started, waiting for initial snapshot");

    try {
        auto snapshot = snapshotFuture_.get();
        reproduceSnapshot(snapshot);
    } catch (const std::exception& e) {
        spdlog::error("engineThread: snapshot reproduction failed: {}", e.what());
        networkThread_.request_stop();
        return;
    } catch (...) {
        spdlog::error("engineThread: snapshot reproduction failed");
        networkThread_.request_stop();
        return;
    }
    spdlog::info("engineThread: snapshot reproduced - consuming ingress queue");

    std::uint64_t applied = 0;
    auto nextDepthPublication = std::chrono::steady_clock::now();
    while (!stopToken.stop_requested()) {
        bool drained = false;
        core::IngressEvent event;
        while (ingress_.try_pop(event)) {
            event.sequence = applied + 1;
            applyIngressEvent(event);
            drained = true;
            ++applied;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextDepthPublication) {
            publishDepthSnapshot();
            nextDepthPublication = now + std::chrono::milliseconds(33);
        }
        if (!drained) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    spdlog::info("engineThread: stop requested, exiting (applied={})", applied);
}

void MainProcess::gatewayLoop(std::stop_token stopToken) {
    spdlog::info("gatewayThread: started");

    constexpr std::size_t kMaxEventsPerIteration = 256;
    std::uint64_t routedEvents = 0;
    std::uint64_t depthPublications = 0;
    std::uint64_t lastDepthVersion = 0;
    std::uint64_t lastDepthSequence = 0;
    egress::DepthSnapshot depth;

    try {
        gateway::TcpGateway gateway(
            [this](const core::IngressEvent& event) {
                return tryEnqueueClientEvent(event);
            });

        while (!stopToken.stop_requested()) {
            // Preserve bounded fairness between private events, public depth,
            // and socket readiness processing.
            std::size_t drained = 0;
            while (drained < kMaxEventsPerIteration && clientEgress_.front()) {
                gateway.route(*clientEgress_.front());
                clientEgress_.pop();
                ++drained;
                ++routedEvents;
            }

            if (depthMailbox_.tryReadNew(lastDepthVersion, depth)) {
                gateway.broadcastDepth(depth);
                lastDepthSequence = depth.sequence;
                ++depthPublications;
            }

            gateway.pollOnce(std::chrono::milliseconds(1));
        }
    } catch (const std::exception& e) {
        spdlog::error("gatewayThread: fatal error: {}", e.what());
    }

    // Producers have already stopped because of member destruction order.
    while (clientEgress_.front()) {
        clientEgress_.pop();
        ++routedEvents;
    }
    if (depthMailbox_.tryReadNew(lastDepthVersion, depth)) {
        lastDepthSequence = depth.sequence;
        ++depthPublications;
    }

    spdlog::info("gatewayThread: stop requested, exiting "
                 "(client_events={}, depth_publications={}, last_depth_seq={})",
                 routedEvents, depthPublications, lastDepthSequence);
}

} // namespace babo
