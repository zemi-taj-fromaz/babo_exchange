#include "feed/bitstamp_ws.hpp"

#include "core/logging.hpp"
#include "feed/fixed_point.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace babo::feed {

namespace {
constexpr const char* kUrl = "wss://ws.bitstamp.net";

std::string findCaBundle() {
#if defined(_WIN32)
    // IXWebSocket's mbedTLS backend imports the Windows certificate store when
    // the special SYSTEM value is used.
    return "SYSTEM";
#else
    // mbedTLS cannot query the macOS Keychain or Linux trust store through
    // IXWebSocket's SYSTEM path. Respect the conventional override first,
    // then probe common bundle locations used by macOS and Linux distros.
    if (const char* configured = std::getenv("SSL_CERT_FILE");
        configured != nullptr && *configured != '\0' &&
        std::filesystem::is_regular_file(configured)) {
        return configured;
    }

    constexpr std::array<std::string_view, 5> candidates{
        "/etc/ssl/cert.pem",                         // macOS, Alpine
        "/etc/ssl/certs/ca-certificates.crt",       // Debian, Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",         // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                   // openSUSE
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
    };
    for (const auto candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) {
            return std::string(candidate);
        }
    }
    throw std::runtime_error(
        "no TLS CA bundle found; set SSL_CERT_FILE to a PEM bundle");
#endif
}
} // namespace

BitstampFeed::BitstampFeed(std::string pair)
    : pair_(std::move(pair)), channel_("live_orders_" + pair_) {
    ix::initNetSystem(); // WSAStartup on Windows; no-op elsewhere (ref-counted)
    ws_.setUrl(kUrl);
    ix::SocketTLSOptions tls;
    tls.caFile = findCaBundle();
    ws_.setTLSOptions(tls);
    spdlog::info("bitstamp ws: TLS CA source={}", tls.caFile);
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
        order.price_ticks = parsePriceTicks(price);
        order.qty_lots = parseQtyLots(amount);
    } catch (const std::exception& e) {
        spdlog::warn("bitstamp ws: bad order payload: {}", e.what());
        return;
    }
    order.side = side;

    if (event == "order_created") {
        order.type = OrderEventType::New;
    } else if (event == "order_changed") {
        order.type = OrderEventType::Modify;
    } else { // order_deleted
        order.type = OrderEventType::Cancel;
    }

    if (orderHandler_) {
        orderHandler_(order);
    }

    if ((++total_ % 2000) == 0) {
        spdlog::info("bitstamp ws: {} order events seen", total_);
    }
}

} // namespace babo::feed
