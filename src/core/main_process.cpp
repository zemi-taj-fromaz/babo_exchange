#include "core/main_process.hpp"

#include "core/logging.hpp"
#include "feed/bitstamp_ws.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace babo {

MainProcess::MainProcess()
    : snapshotFuture_(snapshotPromise_.get_future()), ingress_(1u << 16),
      engineThread_([this](std::stop_token st) { engineLoop(st); }),
      networkThread_([this](std::stop_token st) { networkLoop(st); }) {
    spdlog::info("MainProcess constructed - networkThread + engineThread launched");
}

namespace {
constexpr double kPriceScale = 100.0;         // USD -> integer cents
constexpr double kQtyScale = 100'000'000.0;   // BTC -> integer satoshis

double toDisplayPrice(std::uint64_t price_ticks) {
    return static_cast<double>(price_ticks) / kPriceScale;
}

double toDisplayQty(std::uint64_t qty_lots) {
    return static_cast<double>(qty_lots) / kQtyScale;
}

std::string bar(std::uint64_t qty, std::uint64_t max_qty) {
    if (qty == 0 || max_qty == 0) {
        return {};
    }
    const auto width = static_cast<std::size_t>(
        std::max<std::uint64_t>(1, (qty * 28) / max_qty));
    return std::string(width, '#');
}
} // namespace

std::size_t MainProcess::seedSide(const std::vector<feed::RestingOrder>& orders,
                                  bool is_buy, std::size_t& skipped) {
    std::size_t seeded = 0;
    for (const auto& ro : orders) {
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

void MainProcess::enqueueOrderEvent(const feed::OrderEvent& event) {
    ingress_.push(event);
}

void MainProcess::applyOrderEvent(const feed::OrderEvent& event) {
    const auto price_ticks = event.price_ticks;
    const auto qty_lots = event.qty_lots;

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
        book_.add(order);
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

void MainProcess::renderDepth(std::uint64_t appliedEvents) {
    auto& depth = book_.depth();
    const auto* bids = depth.bids();
    const auto* asks = depth.asks();

    std::uint64_t max_qty = 0;
    for (int i = 0; i < 5; ++i) {
        max_qty = std::max(max_qty, bids[i].aggregate_qty());
        max_qty = std::max(max_qty, asks[i].aggregate_qty());
    }

    std::ostringstream out;
    out << "\x1b[2J\x1b[H";
    out << "babo_exchange BTC/USD depth"
        << " | applied events: " << appliedEvents << "\n";
    if (bids[0].price() != book::INVALID_LEVEL_PRICE &&
        asks[0].price() != book::INVALID_LEVEL_PRICE &&
        bids[0].price() >= asks[0].price()) {
        out << "WARNING: crossed book detected"
            << " best_bid=" << std::fixed << std::setprecision(2)
            << toDisplayPrice(bids[0].price())
            << " best_ask=" << toDisplayPrice(asks[0].price()) << "\n";
    }
    out << "---------------------------------------------------------------\n";
    out << " side        price          qty      orders  depth\n";
    out << "---------------------------------------------------------------\n";

    for (int i = 4; i >= 0; --i) {
        if (asks[i].price() == book::INVALID_LEVEL_PRICE) {
            continue;
        }
        out << " ASK  " << std::setw(12) << std::fixed << std::setprecision(2)
            << toDisplayPrice(asks[i].price())
            << "  " << std::setw(11) << std::setprecision(8)
            << toDisplayQty(asks[i].aggregate_qty())
            << "  " << std::setw(6) << asks[i].order_count()
            << "  " << bar(asks[i].aggregate_qty(), max_qty) << "\n";
    }

    out << "-------------------------- spread -----------------------------\n";

    for (int i = 0; i < 5; ++i) {
        if (bids[i].price() == book::INVALID_LEVEL_PRICE) {
            continue;
        }
        out << " BID  " << std::setw(12) << std::fixed << std::setprecision(2)
            << toDisplayPrice(bids[i].price())
            << "  " << std::setw(11) << std::setprecision(8)
            << toDisplayQty(bids[i].aggregate_qty())
            << "  " << std::setw(6) << bids[i].order_count()
            << "  " << bar(bids[i].aggregate_qty(), max_qty) << "\n";
    }

    out << "\nCtrl+C to stop\n";
    std::cout << out.str() << std::flush;
}

void MainProcess::networkLoop(std::stop_token stopToken) {
    spdlog::info("networkThread: started");

    bool snapshotPublished = false;
    try {
        // The network thread fetches data; the engine thread exclusively owns
        // reconstruction and every subsequent mutation of the matching book.
        auto snapshot = feed::fetchL3Snapshot("btcusd");
        spdlog::info("networkThread: fetched L3 snapshot microtimestamp={} "
                     "({} bids, {} asks)",
                     snapshot.microtimestamp, snapshot.bids.size(),
                     snapshot.asks.size());
        snapshotPromise_.set_value(std::move(snapshot));
        snapshotPublished = true;

        // Showcase trade-off: live subscription starts after the snapshot, so a
        // small bootstrap gap is possible. Keeping that limitation explicit
        // avoids coupling the clean ingress architecture to recovery logic.
        feed::BitstampFeed liveFeed("btcusd");
        liveFeed.setOrderHandler(
            [this](const feed::OrderEvent& event) { enqueueOrderEvent(event); });
        liveFeed.start();

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

    std::cout << "\x1b[?25l" << std::flush;
    std::uint64_t applied = 0;
    auto nextRender = std::chrono::steady_clock::now();
    while (!stopToken.stop_requested()) {
        bool drained = false;
        while (auto* event = ingress_.front()) {
            applyOrderEvent(*event);
            ingress_.pop();
            drained = true;
            ++applied;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextRender) {
            renderDepth(applied);
            nextRender = now + std::chrono::milliseconds(33);
        }
        if (!drained) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    std::cout << "\x1b[?25h" << std::flush;
    spdlog::info("engineThread: stop requested, exiting (applied={})", applied);
}

} // namespace babo
