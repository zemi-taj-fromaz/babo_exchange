#include "core/main_process.hpp"

#include "core/logging.hpp"
#include "feed/bitstamp_ws.hpp"

#include <chrono>
#include <cstdint>
#include <utility>

namespace babo {

MainProcess::MainProcess()
    : ingress_(1u << 16),
      networkThread_([this](std::stop_token st) { networkLoop(st); }),
      engineThread_([this](std::stop_token st) { engineLoop(st); }) {
    spdlog::info("MainProcess constructed - networkThread + engineThread launched");
}

namespace {
constexpr double kPriceScale = 100.0;         // USD -> integer cents
constexpr double kQtyScale = 100'000'000.0;   // BTC -> integer satoshis

std::uint64_t toTicks(double price) {
    const double px = price * kPriceScale;
    return px < 1.0 ? 0 : static_cast<std::uint64_t>(px + 0.5);
}

std::uint64_t toLots(double size) {
    const double qty = size * kQtyScale;
    return qty < 1.0 ? 0 : static_cast<std::uint64_t>(qty + 0.5);
}
} // namespace

std::size_t MainProcess::seedSide(const std::vector<feed::RestingOrder>& orders,
                                  bool is_buy, std::size_t& skipped) {
    std::size_t seeded = 0;
    for (const auto& ro : orders) {
        const auto price_ticks = toTicks(ro.price);
        const auto qty_lots = toLots(ro.size);
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

void MainProcess::reproduceSnapshot(feed::L3Snapshot snapshot) {
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

void MainProcess::enqueueOrderEvent(const feed::OrderEvent& event) {
    ingress_.push(event);
}

void MainProcess::applyOrderEvent(const feed::OrderEvent& event) {
    const auto price_ticks = toTicks(event.price);
    const auto qty_lots = toLots(event.size);

    switch (event.type) {
    case feed::OrderEventType::New: {
        if (price_ticks == 0 || qty_lots == 0) {
            return;
        }
        if (book_.bids().find_order(event.order_id) ||
            book_.asks().find_order(event.order_id)) {
            spdlog::warn("engine: duplicate NEW id={} ignored", event.order_id);
            return;
        }

        simple::SimpleOrder order(event.side == 'B', price_ticks, qty_lots);
        order.order_id_ = event.order_id;
        if (order.is_buy()) {
            book_.bids().insert(order);
        } else {
            book_.asks().insert(order);
        }
        break;
    }
    case feed::OrderEventType::Modify: {
        simple::SimpleOrder* existing = book_.bids().find_order(event.order_id);
        if (!existing) {
            existing = book_.asks().find_order(event.order_id);
        }
        if (!existing) {
            spdlog::warn("engine: MODIFY unknown id={} ignored", event.order_id);
            return;
        }
        if (qty_lots == 0) {
            book_.cancel(event.order_id);
            return;
        }

        const auto new_price =
            (price_ticks == existing->price()) ? book::PRICE_UNCHANGED : price_ticks;
        const auto size_delta =
            static_cast<std::int64_t>(qty_lots) -
            static_cast<std::int64_t>(existing->open_qty());
        book_.replace(event.order_id, size_delta, new_price);
        break;
    }
    case feed::OrderEventType::Cancel:
        book_.cancel(event.order_id);
        break;
    case feed::OrderEventType::Match:
        break;
    }
}

void MainProcess::networkLoop(std::stop_token stopToken) {
    spdlog::info("networkThread: started");

    feed::L3Snapshot snapshot;
    try {
        snapshot = feed::fetchL3Snapshot("btcusd");
        spdlog::info("networkThread: fetched L3 snapshot microtimestamp={} "
                     "({} bids, {} asks)",
                     snapshot.microtimestamp, snapshot.bids.size(),
                     snapshot.asks.size());
    } catch (const std::exception& e) {
        spdlog::error("networkThread: snapshot fetch failed: {}", e.what());
    }

    reproduceSnapshotFuture_ = std::async(
        std::launch::async, [this, snapshot = std::move(snapshot)]() mutable {
            reproduceSnapshot(std::move(snapshot));
        });
    snapshotFuturePublished_.count_down();

    feed::BitstampFeed liveFeed("btcusd");
    liveFeed.setOrderHandler(
        [this](const feed::OrderEvent& event) { enqueueOrderEvent(event); });
    liveFeed.start();

    while (!stopToken.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    liveFeed.stop();
    spdlog::info("networkThread: stop requested, exiting");
}

void MainProcess::engineLoop(std::stop_token stopToken) {
    spdlog::info("engineThread: started, waiting for snapshot future");

    snapshotFuturePublished_.wait();
    reproduceSnapshotFuture_.get();
    spdlog::info("engineThread: snapshot reproduced - consuming ingress queue");

    std::uint64_t applied = 0;
    while (!stopToken.stop_requested()) {
        bool drained = false;
        while (auto* event = ingress_.front()) {
            applyOrderEvent(*event);
            ingress_.pop();
            drained = true;
            ++applied;
            if ((applied % 2000) == 0) {
                spdlog::info("engineThread: applied {} live order events", applied);
            }
        }
        if (!drained) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    spdlog::info("engineThread: stop requested, exiting");
}

} // namespace babo
