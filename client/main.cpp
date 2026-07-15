#include "gateway_connection.hpp"
#include "feed/fixed_point.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::size_t kDepthLevels = 5;
constexpr std::size_t kMaxVisibleActiveOrders = 6;
constexpr double kPriceScale = 100.0;
constexpr std::uint64_t kQtyScale = 100'000'000;

struct DepthLevel {
    std::uint64_t priceTicks = 0;
    std::uint64_t qtyLots = 0;
    std::uint32_t orderCount = 0;
};

struct ActiveOrder {
    std::uint64_t clientOrderId = 0;
    std::uint64_t exchangeOrderId = 0;
    char side = '?';
    std::uint64_t priceTicks = 0;
    std::uint64_t remainingQtyLots = 0;
};

struct ClientState {
    bool connected = true;
    std::uint64_t sessionId = 0;
    std::uint64_t depthSequence = 0;
    std::uint64_t orderEventSequence = 0;
    std::array<DepthLevel, kDepthLevels> bids{};
    std::array<DepthLevel, kDepthLevels> asks{};
    std::unordered_map<std::uint64_t, ActiveOrder> activeOrders;
    double usdBalance = 100'000.0;
    double btcBalance = 0.0;
    std::string connectionStatus = "Connected - waiting for market data";
    std::string activity = "Ready";
};

enum class ReportType {
    Accepted,
    Rejected,
    Cancelled,
    CancelRejected,
    Replaced,
    ReplaceRejected,
    Fill,
};

struct OrderReport {
    ReportType type{};
    std::uint64_t sequence = 0;
    std::uint64_t clientOrderId = 0;
    std::uint64_t exchangeOrderId = 0;
    char side = '?';
    std::uint64_t priceTicks = 0;
    std::uint64_t qtyLots = 0;
    std::uint64_t remainingQtyLots = 0;
};

template <typename Integer>
bool parseInteger(std::string_view input, Integer& result) {
    if (input.empty()) return false;
    const auto* begin = input.data();
    const auto* end = begin + input.size();
    const auto [ptr, error] = std::from_chars(begin, end, result);
    return error == std::errc{} && ptr == end;
}

bool parseLevel(std::string_view input, DepthLevel& level) {
    const auto firstComma = input.find(',');
    if (firstComma == std::string_view::npos) return false;
    const auto secondComma = input.find(',', firstComma + 1);
    if (secondComma == std::string_view::npos ||
        input.find(',', secondComma + 1) != std::string_view::npos) {
        return false;
    }

    return parseInteger(input.substr(0, firstComma), level.priceTicks) &&
           parseInteger(input.substr(firstComma + 1,
                                     secondComma - firstComma - 1),
                        level.qtyLots) &&
           parseInteger(input.substr(secondComma + 1), level.orderCount);
}

bool reportType(std::string_view name, ReportType& type) {
    if (name == "ACCEPTED") type = ReportType::Accepted;
    else if (name == "REJECTED") type = ReportType::Rejected;
    else if (name == "CANCELLED") type = ReportType::Cancelled;
    else if (name == "CANCEL_REJECTED") type = ReportType::CancelRejected;
    else if (name == "REPLACED") type = ReportType::Replaced;
    else if (name == "REPLACE_REJECTED") type = ReportType::ReplaceRejected;
    else if (name == "FILL") type = ReportType::Fill;
    else return false;
    return true;
}

bool parseOrderReport(std::string_view line, OrderReport& report) {
    const auto firstSpace = line.find(' ');
    const auto name = line.substr(0, firstSpace);
    if (!reportType(name, report.type) || firstSpace == std::string_view::npos) {
        return false;
    }

    bool hasSequence = false;
    bool hasClientOrderId = false;
    bool hasExchangeOrderId = false;
    std::size_t cursor = firstSpace + 1;
    while (cursor < line.size()) {
        const auto end = line.find(' ', cursor);
        const auto token = line.substr(
            cursor, end == std::string_view::npos ? line.size() - cursor
                                                  : end - cursor);
        const auto equals = token.find('=');
        if (equals == std::string_view::npos) return false;
        const auto key = token.substr(0, equals);
        const auto value = token.substr(equals + 1);

        if (key == "seq") {
            hasSequence = parseInteger(value, report.sequence);
        } else if (key == "client_order_id") {
            hasClientOrderId = parseInteger(value, report.clientOrderId);
        } else if (key == "order_id") {
            hasExchangeOrderId = parseInteger(value, report.exchangeOrderId);
        } else if (key == "side") {
            if (value.size() != 1 ||
                (value[0] != 'B' && value[0] != 'S' && value[0] != '-')) {
                return false;
            }
            report.side = value[0] == '-' ? '?' : value[0];
        } else if (key == "price_ticks") {
            if (!parseInteger(value, report.priceTicks)) return false;
        } else if (key == "qty_lots") {
            if (!parseInteger(value, report.qtyLots)) return false;
        } else if (key == "remaining_qty_lots") {
            if (!parseInteger(value, report.remainingQtyLots)) return false;
        } else if (key != "role" && key != "reason") {
            return false;
        }

        if (end == std::string_view::npos) break;
        cursor = end + 1;
    }
    return hasSequence && hasClientOrderId && hasExchangeOrderId;
}

void applyOrderReport(const OrderReport& report, ClientState& state) {
    state.orderEventSequence = report.sequence;
    auto active = state.activeOrders.find(report.clientOrderId);

    switch (report.type) {
    case ReportType::Accepted:
        state.activeOrders[report.clientOrderId] = ActiveOrder{
            report.clientOrderId, report.exchangeOrderId, report.side,
            report.priceTicks, report.remainingQtyLots};
        state.activity = "Order " + std::to_string(report.clientOrderId) +
                         " accepted";
        break;
    case ReportType::Fill: {
        const double btc = static_cast<double>(report.qtyLots) / kQtyScale;
        const double usd = btc *
                           (static_cast<double>(report.priceTicks) / kPriceScale);
        if (report.side == 'B') {
            state.usdBalance -= usd;
            state.btcBalance += btc;
        } else {
            state.usdBalance += usd;
            state.btcBalance -= btc;
        }
        if (active != state.activeOrders.end()) {
            active->second.remainingQtyLots = report.remainingQtyLots;
            if (report.remainingQtyLots == 0) {
                state.activeOrders.erase(active);
            }
        }
        state.activity = "Order " + std::to_string(report.clientOrderId) +
                         " filled " + std::to_string(report.qtyLots) + " lots";
        break;
    }
    case ReportType::Cancelled:
    case ReportType::Rejected:
        if (active != state.activeOrders.end()) {
            state.activeOrders.erase(active);
        }
        state.activity = "Order " + std::to_string(report.clientOrderId) +
                         (report.type == ReportType::Cancelled ? " cancelled"
                                                               : " rejected");
        break;
    case ReportType::Replaced:
        if (active != state.activeOrders.end()) {
            active->second.priceTicks = report.priceTicks;
            active->second.remainingQtyLots = report.remainingQtyLots;
        }
        state.activity = "Order " + std::to_string(report.clientOrderId) +
                         " replaced";
        break;
    case ReportType::CancelRejected:
        state.activity = "Cancel rejected for order " +
                         std::to_string(report.clientOrderId);
        break;
    case ReportType::ReplaceRejected:
        state.activity = "Replace rejected for order " +
                         std::to_string(report.clientOrderId);
        break;
    }
}

bool applyDepth(std::string_view line, ClientState& state) {
    std::array<DepthLevel, kDepthLevels> bids{};
    std::array<DepthLevel, kDepthLevels> asks{};
    std::uint64_t sequence = 0;
    std::size_t bidCount = 0;
    std::size_t askCount = 0;
    bool hasSequence = false;
    std::size_t cursor = 6;

    while (cursor < line.size()) {
        const auto end = line.find(' ', cursor);
        const auto token = line.substr(
            cursor, end == std::string_view::npos ? line.size() - cursor
                                                  : end - cursor);

        if (token.starts_with("seq=")) {
            hasSequence = parseInteger(token.substr(4), sequence);
        } else if (token.starts_with("B=") && bidCount < kDepthLevels) {
            if (!parseLevel(token.substr(2), bids[bidCount++])) return false;
        } else if (token.starts_with("A=") && askCount < kDepthLevels) {
            if (!parseLevel(token.substr(2), asks[askCount++])) return false;
        } else {
            return false;
        }

        if (end == std::string_view::npos) break;
        cursor = end + 1;
    }

    if (!hasSequence || bidCount != kDepthLevels || askCount != kDepthLevels) {
        return false;
    }
    state.depthSequence = sequence;
    state.bids = bids;
    state.asks = asks;
    state.connectionStatus = "Live";
    return true;
}

bool applyGatewayLine(std::string_view line, ClientState& state) {
    constexpr std::string_view sessionPrefix = "SESSION ";
    if (line.starts_with(sessionPrefix)) {
        if (!parseInteger(line.substr(sessionPrefix.size()), state.sessionId)) {
            state.activity = "Received malformed SESSION message";
        }
        return true;
    }
    if (line.starts_with("COMMANDS ")) return true;
    if (line.starts_with("ERROR ")) {
        state.activity = std::string(line);
        return true;
    }
    if (line.starts_with("DEPTH ")) {
        if (!applyDepth(line, state)) {
            state.activity = "Received malformed DEPTH message";
        }
        return true;
    }

    OrderReport report;
    if (parseOrderReport(line, report)) {
        applyOrderReport(report, state);
        return true;
    }
    state.activity = "Unknown gateway message";
    return false;
}

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

bool parseDollarNotional(std::string_view text, std::uint64_t& ticks) {
    const bool prefixed = !text.empty() && text.front() == '$';
    const bool suffixed = !text.empty() && text.back() == '$';
    if (prefixed == suffixed) {
        return false;
    }
    if (prefixed) {
        text.remove_prefix(1);
    } else {
        text.remove_suffix(1);
    }
    try {
        ticks = babo::feed::parsePriceTicks(text);
        return ticks != 0;
    } catch (const std::exception&) {
        return false;
    }
}

std::string fixedDecimal(std::uint64_t value, std::uint64_t scale,
                         int fractionalDigits) {
    std::ostringstream output;
    output << value / scale << '.' << std::setfill('0')
           << std::setw(fractionalDigits) << value % scale;
    return output.str();
}

std::string dollarPrice(double ticks) {
    std::ostringstream output;
    output << '$' << std::fixed << std::setprecision(2) << ticks / kPriceScale;
    return output.str();
}

std::string dollars(double value) {
    std::ostringstream output;
    if (value < 0.0) {
        output << "-$" << std::fixed << std::setprecision(2) << -value;
    } else {
        output << '$' << std::fixed << std::setprecision(2) << value;
    }
    return output.str();
}

std::string btc(double value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(8) << value << " BTC";
    return output.str();
}

std::string depthRow(std::string_view side, const DepthLevel& level) {
    std::ostringstream row;
    row << std::left << std::setw(6) << side << std::right << std::setw(15)
        << fixedDecimal(level.priceTicks, 100, 2) << std::setw(18)
        << fixedDecimal(level.qtyLots, 100'000'000, 8) << std::setw(10)
        << level.orderCount;
    return row.str();
}

ftxui::Element renderMarketSummary(const ClientState& state) {
    using namespace ftxui;
    const auto bestBid = state.bids[0].priceTicks;
    const auto bestAsk = state.asks[0].priceTicks;
    if (bestBid == 0 || bestAsk == 0) {
        return hbox({text("MID: --") | bold, filler(), text("Spread: --")});
    }

    const double midTicks =
        (static_cast<double>(bestBid) + static_cast<double>(bestAsk)) / 2.0;
    const bool crossed = bestBid >= bestAsk;
    const auto spreadTicks = crossed ? bestBid - bestAsk : bestAsk - bestBid;
    return hbox({
        text("MID: " + dollarPrice(midTicks)) | bold,
        filler(),
        text("Bid: " + dollarPrice(static_cast<double>(bestBid))) |
            color(Color::Green),
        text("  "),
        text("Ask: " + dollarPrice(static_cast<double>(bestAsk))) |
            color(Color::Red),
        text("  "),
        text(std::string(crossed ? "Crossed: " : "Spread: ") +
             dollarPrice(static_cast<double>(spreadTicks))) |
            color(crossed ? Color::Yellow : Color::White),
    });
}

ftxui::Elements renderActiveOrders(const ClientState& state) {
    using namespace ftxui;
    std::vector<ActiveOrder> orders;
    orders.reserve(state.activeOrders.size());
    for (const auto& [id, order] : state.activeOrders) orders.push_back(order);
    std::sort(orders.begin(), orders.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.clientOrderId < rhs.clientOrderId;
    });

    Elements rows;
    rows.push_back(text("LOCAL  SIDE           PRICE          REMAINING       EXCHANGE ID") |
                   bold);
    if (orders.empty()) {
        rows.push_back(text("No active orders") | dim);
        return rows;
    }
    const auto visible = std::min(orders.size(), kMaxVisibleActiveOrders);
    for (std::size_t i = 0; i < visible; ++i) {
        const auto& order = orders[i];
        std::ostringstream row;
        row << std::left << std::setw(7) << order.clientOrderId << std::setw(7)
            << (order.side == 'B' ? "BUY" : "SELL") << std::right
            << std::setw(15)
            << (order.priceTicks == 0
                    ? std::string("MARKET")
                    : fixedDecimal(order.priceTicks, 100, 2))
            << std::setw(19)
            << fixedDecimal(order.remainingQtyLots, 100'000'000, 8)
            << std::setw(20) << order.exchangeOrderId;
        rows.push_back(text(row.str()) |
                       color(order.side == 'B' ? Color::Green : Color::Red));
    }
    if (orders.size() > visible) {
        rows.push_back(text("... " + std::to_string(orders.size() - visible) +
                            " more active orders") |
                       dim);
    }
    return rows;
}

ftxui::Element renderScreen(const ClientState& state,
                            const ftxui::Component& commandInput,
                            const ftxui::Component& submitButton) {
    using namespace ftxui;
    Elements depthRows;
    depthRows.push_back(text("SIDE            PRICE              SIZE    ORDERS") | bold);
    depthRows.push_back(separator());
    for (std::size_t i = kDepthLevels; i-- > 0;) {
        depthRows.push_back(text(depthRow("ASK", state.asks[i])) |
                            color(Color::Red));
    }
    depthRows.push_back(separator());
    for (std::size_t i = 0; i < kDepthLevels; ++i) {
        depthRows.push_back(text(depthRow("BID", state.bids[i])) |
                            color(Color::Green));
    }

    const std::string session = state.sessionId == 0
                                    ? "pending"
                                    : std::to_string(state.sessionId);
    double markPrice = 0.0;
    if (state.bids[0].priceTicks != 0 && state.asks[0].priceTicks != 0) {
        markPrice =
            (static_cast<double>(state.bids[0].priceTicks) +
             static_cast<double>(state.asks[0].priceTicks)) /
            (2.0 * kPriceScale);
    }
    const double btcPositionValue = state.btcBalance * markPrice;
    const double totalAccountValue = state.usdBalance + btcPositionValue;
    auto activeRows = renderActiveOrders(state);
    return vbox({
               hbox({text(" babo_client ") | bold, filler(),
                     text(state.connectionStatus) |
                         color(state.connected ? Color::Green : Color::Red)}),
               separator(),
               hbox({text("Session: " + session), text("  "),
                     text("Depth: " + std::to_string(state.depthSequence)),
                     text("  "),
                     text("Orders: " +
                          std::to_string(state.orderEventSequence)),
                     filler()}),
               renderMarketSummary(state),
               separator(),
               vbox(std::move(depthRows)) | center,
               separator(),
               text("ACTIVE ORDERS") | bold,
               vbox(std::move(activeRows)),
               separator(),
               hbox({text("TOTAL " + dollars(totalAccountValue)) | bold,
                     text("    "),
                     text("BTC " + dollars(btcPositionValue) + " (" +
                          btc(state.btcBalance) + ")") |
                         color(state.btcBalance >= 0.0 ? Color::Green
                                                      : Color::Red),
                     text("    "),
                     text("CASH " + dollars(state.usdBalance)) | bold}),
               hbox({text("> "), commandInput->Render() | flex,
                     text("  "), submitButton->Render()}),
               text(state.activity) | dim,
           }) |
           border;
}

} // namespace

int runClient() {
    using namespace babo::client;
    using namespace ftxui;

    GatewayConnection connection("127.0.0.1", 9000);
    ScreenInteractive screen = ScreenInteractive::Fullscreen();
    ClientState state;
    std::mutex stateMutex;
    std::string command;

    auto submitCommand = [&] {
        const auto line = trim(command);
        if (line.empty()) return;

        std::string outbound = line;
        std::istringstream input(line);
        std::vector<std::string> parts;
        for (std::string part; input >> part;) parts.push_back(std::move(part));
        const auto& verb = parts.front();
        if (verb == "cancel") {
            std::uint64_t localId = 0;
            if (parts.size() != 2 || !parseInteger(parts[1], localId)) {
                std::scoped_lock lock(stateMutex);
                state.activity = "Usage: cancel <local-order-id>";
                return;
            }
            std::uint64_t exchangeId = 0;
            {
                std::scoped_lock lock(stateMutex);
                const auto active = state.activeOrders.find(localId);
                if (active == state.activeOrders.end()) {
                    state.activity = "Unknown active local order " + parts[1];
                    return;
                }
                exchangeId = active->second.exchangeOrderId;
            }
            outbound = "cancel " + std::to_string(exchangeId);
        } else if ((verb == "buy" || verb == "sell") &&
                   parts.size() == 2 && parts[1] == "all") {
            double balance = 0.0;
            {
                std::scoped_lock lock(stateMutex);
                balance = verb == "buy" ? state.usdBalance
                                         : state.btcBalance;
            }
            if (balance <= 0.0) {
                std::scoped_lock lock(stateMutex);
                state.activity = verb == "buy" ? "No cash available to buy"
                                                 : "No BTC available to sell";
                return;
            }

            if (verb == "buy") {
                const double scaled = balance * kPriceScale;
                if (scaled >=
                    static_cast<double>(
                        std::numeric_limits<std::uint64_t>::max())) {
                    std::scoped_lock lock(stateMutex);
                    state.activity = "Cash balance is too large";
                    return;
                }
                const auto cashTicks =
                    static_cast<std::uint64_t>(std::floor(scaled + 1e-7));
                if (cashTicks == 0) {
                    std::scoped_lock lock(stateMutex);
                    state.activity = "Cash balance is below one cent";
                    return;
                }
                outbound = "buy " + fixedDecimal(cashTicks, 100, 2);
            } else {
                const double scaled = balance * static_cast<double>(kQtyScale);
                if (scaled >=
                    static_cast<double>(
                        std::numeric_limits<std::uint64_t>::max())) {
                    std::scoped_lock lock(stateMutex);
                    state.activity = "BTC balance is too large";
                    return;
                }
                const auto qtyLots = static_cast<std::uint64_t>(
                    std::floor(scaled + 0.5));
                if (qtyLots == 0) {
                    std::scoped_lock lock(stateMutex);
                    state.activity = "BTC balance is below one lot";
                    return;
                }
                outbound = "sell " + fixedDecimal(qtyLots, kQtyScale, 8);
            }
        } else if ((verb == "buy" || verb == "sell") &&
                   parts.size() == 3 &&
                   (parts[1].starts_with('$') || parts[1].ends_with('$'))) {
            std::uint64_t notionalTicks = 0;
            std::uint64_t limitPriceTicks = 0;
            try {
                if (!parseDollarNotional(parts[1], notionalTicks)) {
                    throw std::invalid_argument("notional");
                }
                limitPriceTicks = babo::feed::parsePriceTicks(parts[2]);
            } catch (const std::exception&) {
                std::scoped_lock lock(stateMutex);
                state.activity = "Usage: buy|sell <usd$> <limit-price>";
                return;
            }
            if (limitPriceTicks == 0 ||
                notionalTicks >
                    std::numeric_limits<std::uint64_t>::max() / kQtyScale) {
                std::scoped_lock lock(stateMutex);
                state.activity = "Invalid dollar limit order";
                return;
            }
            const auto qtyLots =
                notionalTicks * kQtyScale / limitPriceTicks;
            if (qtyLots == 0) {
                std::scoped_lock lock(stateMutex);
                state.activity = "Dollar notional is below one BTC lot";
                return;
            }
            outbound = verb + " " + fixedDecimal(qtyLots, kQtyScale, 8) +
                       " " + fixedDecimal(limitPriceTicks, 100, 2);
        } else if (verb == "sell" && parts.size() == 2) {
            std::uint64_t usdCents = 0;
            try {
                usdCents = babo::feed::parsePriceTicks(parts[1]);
            } catch (const std::exception&) {
                std::scoped_lock lock(stateMutex);
                state.activity = "Usage: sell <usd>";
                return;
            }

            std::uint64_t bestBid = 0;
            {
                std::scoped_lock lock(stateMutex);
                bestBid = state.bids[0].priceTicks;
            }
            if (usdCents == 0 || bestBid == 0 ||
                usdCents >
                    std::numeric_limits<std::uint64_t>::max() / kQtyScale) {
                std::scoped_lock lock(stateMutex);
                state.activity = bestBid == 0 ? "No live bid for market sell"
                                              : "Invalid sell notional";
                return;
            }
            const auto qtyLots = usdCents * kQtyScale / bestBid;
            if (qtyLots == 0) {
                std::scoped_lock lock(stateMutex);
                state.activity = "Sell notional is below one BTC lot";
                return;
            }
            outbound = "sell " + fixedDecimal(qtyLots, kQtyScale, 8);
        } else if ((verb == "buy" || verb == "sell") &&
                   (parts.size() == 2 || parts.size() == 3)) {
            // buy <usd> is converted by the engine; two-argument commands are
            // limit orders whose quantity is already expressed in BTC.
        } else {
            std::scoped_lock lock(stateMutex);
            state.activity = "Unknown order command";
            return;
        }

        try {
            connection.sendLine(outbound);
            {
                std::scoped_lock lock(stateMutex);
                state.activity = outbound == line
                                     ? "Sent: " + line
                                     : "Sent: " + line + " as " + outbound;
            }
            command.clear();
        } catch (const std::exception& error) {
            std::scoped_lock lock(stateMutex);
            state.activity = error.what();
        }
    };

    InputOption inputOption;
    inputOption.on_enter = submitCommand;
    auto commandInput = Input(
        &command,
        "buy 5000$ 65000 | buy all | sell all | cancel 1",
        inputOption);
    auto submitButton = Button("Submit", submitCommand, ButtonOption::Ascii());
    auto controls = Container::Horizontal({commandInput, submitButton});

    std::jthread networkThread(
        [&](std::stop_token stopToken) {
            connection.run(
                stopToken,
                [&](std::string_view line) {
                    {
                        std::scoped_lock lock(stateMutex);
                        applyGatewayLine(line, state);
                    }
                    screen.PostEvent(Event::Custom);
                },
                [&](std::string_view reason) {
                    {
                        std::scoped_lock lock(stateMutex);
                        state.connected = false;
                        state.connectionStatus = std::string(reason);
                    }
                    screen.PostEvent(Event::Custom);
                });
        });

    auto renderer = Renderer(controls, [&] {
        ClientState snapshot;
        {
            std::scoped_lock lock(stateMutex);
            snapshot = state;
        }
        return renderScreen(snapshot, commandInput, submitButton);
    });

    auto component = CatchEvent(renderer, [&](Event event) {
        if (event == Event::Escape) {
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    screen.Loop(component);
    networkThread.request_stop();
    connection.stop();
    networkThread.join();
    return 0;
}

int main() {
    try {
        return runClient();
    } catch (const std::exception& error) {
        std::cerr << "babo_client: " << error.what() << '\n';
        return 1;
    }
}
