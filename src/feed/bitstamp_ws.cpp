#include "feed/bitstamp_ws.hpp"

#include "core/logging.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <nlohmann/json.hpp>

#include <utility>

namespace babo::feed {

namespace {
constexpr const char* kUrl = "wss://ws.bitstamp.net";
} // namespace

BitstampFeed::BitstampFeed(std::string pair)
    : pair_(std::move(pair)), channel_("live_orders_" + pair_) {
    ix::initNetSystem(); // WSAStartup on Windows; no-op elsewhere (ref-counted)
    ws_.setUrl(kUrl);
    ws_.setOnMessageCallback(
        [this](const ix::WebSocketMessagePtr& msg) { onMessage(msg); });
}

BitstampFeed::~BitstampFeed() {
    stop();
    ix::uninitNetSystem();
}

void BitstampFeed::start() {
    spdlog::info("bitstamp ws: connecting to {} for {}", kUrl, channel_);
    ws_.start(); // spawns IXWebSocket's background thread
}

void BitstampFeed::stop() { ws_.stop(); }

void BitstampFeed::setOrderHandler(OrderHandler handler) {
    orderHandler_ = std::move(handler);
}

void BitstampFeed::onMessage(const ix::WebSocketMessagePtr& msg) {
    switch (msg->type) {
    case ix::WebSocketMessageType::Open: {
        spdlog::info("bitstamp ws: connected — subscribing to {}", channel_);
        // {"event":"bts:subscribe","data":{"channel":"live_orders_btcusd"}}
        nlohmann::json sub;
        sub["event"] = "bts:subscribe";
        sub["data"]["channel"] = channel_;
        ws_.send(sub.dump());
        break;
    }
    case ix::WebSocketMessageType::Message:
        routeEvent(msg->str);
        break;
    case ix::WebSocketMessageType::Error:
        spdlog::error("bitstamp ws: error: {}", msg->errorInfo.reason);
        break;
    case ix::WebSocketMessageType::Close:
        spdlog::warn("bitstamp ws: closed ({}): {}", msg->closeInfo.code,
                     msg->closeInfo.reason);
        break;
    default:
        break;
    }
}

void BitstampFeed::routeEvent(const std::string& payload) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (const std::exception& e) {
        spdlog::warn("bitstamp ws: bad json: {}", e.what());
        return;
    }

    const std::string event = j.value("event", "");

    if (event == "bts:subscription_succeeded") {
        spdlog::info("bitstamp ws: subscription confirmed for {}", channel_);
        return;
    }
    if (event == "bts:request_reconnect") {
        spdlog::warn("bitstamp ws: server requested reconnect");
        return;
    }
    // Only the three order-lifecycle events carry book data.
    if (event != "order_created" && event != "order_changed" &&
        event != "order_deleted") {
        return;
    }
    if (!j.contains("data")) {
        return;
    }

    const nlohmann::json& d = j["data"];
    // order_type: 0 = buy (bid), 1 = sell (ask).
    const char side = (d.value("order_type", 0) == 0) ? 'B' : 'S';
    const std::string id = d.value("id_str", "");
    const std::string price = d.value("price_str", "");
    const std::string amount = d.value("amount_str", "");

    OrderEvent order;
    try {
        order.order_id = std::stoull(id);
        order.price = std::stod(price);
        order.size = std::stod(amount);
    } catch (const std::exception& e) {
        spdlog::warn("bitstamp ws: bad order payload: {}", e.what());
        return;
    }
    order.side = side;

    if (event == "order_created") {
        order.type = OrderEventType::New;
        spdlog::info("NEW    id={} side={} px={} amt={}", id, side, price, amount);
    } else if (event == "order_changed") {
        order.type = OrderEventType::Modify;
        spdlog::info("MODIFY id={} side={} px={} amt={}", id, side, price, amount);
    } else { // order_deleted
        order.type = OrderEventType::Cancel;
        spdlog::info("CANCEL id={} side={} px={} amt={}", id, side, price, amount);
    }

    if (orderHandler_) {
        orderHandler_(order);
    }

    if ((++total_ % 2000) == 0) {
        spdlog::info("bitstamp ws: {} order events seen", total_);
    }
}

} // namespace babo::feed
