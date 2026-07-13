#pragma once

#include "feed/order_event.hpp"

#include <ixwebsocket/IXWebSocket.h>

#include <cstdint>
#include <functional>
#include <string>

namespace babo::feed {

// Streams Bitstamp's public `live_orders_<pair>` L3 channel over a WebSocket.
// Unlike Coinbase's `full` channel, this is PUBLIC — no authentication.
//
// Logging sink for now (does NOT touch the order book): the channel emits
// order_created -> New, order_changed -> Modify, order_deleted -> Cancel. There
// is no match/trade event on this channel (that's the separate live_trades
// channel), so there's nothing to discard here. Order ids are integers, not
// UUIDs. Runs on IXWebSocket's own background thread; start() is non-blocking.
class BitstampFeed {
public:
    using OrderHandler = std::function<void(const OrderEvent&)>;

    explicit BitstampFeed(std::string pair); // e.g. "btcusd"
    ~BitstampFeed();

    BitstampFeed(const BitstampFeed&) = delete;
    BitstampFeed& operator=(const BitstampFeed&) = delete;

    void start(); // connect + subscribe (non-blocking)
    void stop();  // close the connection
    void setOrderHandler(OrderHandler handler);

private:
    void onMessage(const ix::WebSocketMessagePtr& msg);
    void routeEvent(const std::string& payload);

    std::string pair_;    // "btcusd"
    std::string channel_; // "live_orders_btcusd"
    ix::WebSocket ws_;
    OrderHandler orderHandler_;

    std::uint64_t total_ = 0; // order events seen (callback thread only)
};

} // namespace babo::feed
