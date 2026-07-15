#include "gateway_connection.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr std::size_t kDepthLevels = 5;

struct DepthLevel {
    std::uint64_t priceTicks = 0;
    std::uint64_t qtyLots = 0;
    std::uint32_t orderCount = 0;
};

struct ClientState {
    bool connected = true;
    std::uint64_t sessionId = 0;
    std::uint64_t depthSequence = 0;
    std::array<DepthLevel, kDepthLevels> bids{};
    std::array<DepthLevel, kDepthLevels> asks{};
    std::string status = "Connected - waiting for market data";
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

bool applyGatewayLine(std::string_view line, ClientState& state) {
    constexpr std::string_view sessionPrefix = "SESSION ";
    if (line.starts_with(sessionPrefix)) {
        std::uint64_t sessionId = 0;
        if (!parseInteger(line.substr(sessionPrefix.size()), sessionId)) {
            state.status = "Received malformed SESSION message";
            return true;
        }
        state.sessionId = sessionId;
        state.status = "Connected - waiting for depth";
        return true;
    }

    if (!line.starts_with("DEPTH ")) return false;

    ClientState next = state;
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
            hasSequence = parseInteger(token.substr(4), next.depthSequence);
        } else if (token.starts_with("B=") && bidCount < kDepthLevels) {
            if (!parseLevel(token.substr(2), next.bids[bidCount++])) {
                state.status = "Received malformed DEPTH bid";
                return true;
            }
        } else if (token.starts_with("A=") && askCount < kDepthLevels) {
            if (!parseLevel(token.substr(2), next.asks[askCount++])) {
                state.status = "Received malformed DEPTH ask";
                return true;
            }
        } else {
            state.status = "Received malformed DEPTH message";
            return true;
        }

        if (end == std::string_view::npos) break;
        cursor = end + 1;
    }

    if (!hasSequence || bidCount != kDepthLevels ||
        askCount != kDepthLevels) {
        state.status = "Received incomplete DEPTH message";
        return true;
    }

    next.status = "Live";
    state = std::move(next);
    return true;
}

std::string fixedDecimal(std::uint64_t value, std::uint64_t scale,
                         int fractionalDigits) {
    std::ostringstream output;
    output << value / scale << '.' << std::setfill('0')
           << std::setw(fractionalDigits) << value % scale;
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

ftxui::Element renderBook(const ClientState& state) {
    using namespace ftxui;

    Elements rows;
    rows.push_back(text("SIDE            PRICE              SIZE    ORDERS") |
                   bold);
    rows.push_back(separator());

    for (std::size_t i = kDepthLevels; i-- > 0;) {
        rows.push_back(text(depthRow("ASK", state.asks[i])) |
                       color(Color::Red));
    }

    rows.push_back(separator());

    for (std::size_t i = 0; i < kDepthLevels; ++i) {
        rows.push_back(text(depthRow("BID", state.bids[i])) |
                       color(Color::Green));
    }

    const std::string session = state.sessionId == 0
                                    ? "pending"
                                    : std::to_string(state.sessionId);
    auto statusColor = state.connected ? Color::Green : Color::Red;

    return vbox({
               hbox({text(" babo_client ") | bold,
                     filler(),
                     text(state.status) | color(statusColor)}),
               separator(),
               hbox({text("Session: " + session), filler(),
                     text("Depth sequence: " +
                          std::to_string(state.depthSequence))}),
               separator(),
               vbox(std::move(rows)) | center,
               separator(),
               text("q / Esc: quit") | dim | center,
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
                        state.status = std::string(reason);
                    }
                    screen.PostEvent(Event::Custom);
                });
        });

    auto renderer = Renderer([&] {
        ClientState snapshot;
        {
            std::scoped_lock lock(stateMutex);
            snapshot = state;
        }
        return renderBook(snapshot);
    });

    auto component = CatchEvent(renderer, [&](Event event) {
        if (event == Event::Character('q') || event == Event::Escape) {
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
