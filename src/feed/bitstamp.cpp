#include "feed/bitstamp.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace babo::feed {
namespace {

#ifdef _WIN32
#define BABO_POPEN _popen
#define BABO_PCLOSE _pclose
#else
#define BABO_POPEN popen
#define BABO_PCLOSE pclose
#endif

// Run a shell command and capture its stdout. Bootstrap-only helper — this is
// the process boundary we accept for the snapshot (never on the hot path).
std::string runCaptureStdout(const std::string& cmd) {
    FILE* pipe = BABO_POPEN(cmd.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("failed to launch subprocess: " + cmd);
    }
    std::string out;
    std::array<char, 1 << 16> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
        out.append(buf.data(), n);
    }
    BABO_PCLOSE(pipe);
    return out;
}

// Parse one side ("bids" or "asks"): each element is a
// [price_str, amount_str, order_id_str] triple.
void parseSide(const nlohmann::json& arr, char side,
               std::vector<RestingOrder>& out) {
    out.reserve(arr.size());
    for (const auto& e : arr) {
        RestingOrder o;
        o.price = std::stod(e[0].get_ref<const nlohmann::json::string_t&>());
        o.size = std::stod(e[1].get_ref<const nlohmann::json::string_t&>());
        o.order_id = std::stoull(e[2].get_ref<const nlohmann::json::string_t&>());
        o.side = side;
        out.push_back(std::move(o));
    }
}

} // namespace

L3Snapshot fetchL3Snapshot(const std::string& pair) {
    const std::string url =
        "https://www.bitstamp.net/api/v2/order_book/" + pair + "/?group=2";
    // -s: silent progress; -f: fail (empty output + nonzero) on HTTP >= 400.
    const std::string body = runCaptureStdout("curl -sf \"" + url + "\"");
    if (body.empty()) {
        throw std::runtime_error("empty snapshot response for " + pair +
                                 " (curl failed or non-2xx)");
    }

    const auto j = nlohmann::json::parse(body);
    L3Snapshot snap;
    snap.microtimestamp =
        std::stoull(j.at("microtimestamp").get_ref<const nlohmann::json::string_t&>());
    parseSide(j.at("bids"), 'B', snap.bids);
    parseSide(j.at("asks"), 'S', snap.asks);
    return snap;
}

} // namespace babo::feed
