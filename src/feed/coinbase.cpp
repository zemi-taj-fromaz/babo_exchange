#include "feed/coinbase.hpp"

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

// Parse one side ("bids" or "asks") of the snapshot. Each element is a
// [price_str, size_str, order_id_str] triple.
void parseSide(const nlohmann::json& arr, char side,
               std::vector<RestingOrder>& out) {
    out.reserve(arr.size());
    for (const auto& e : arr) {
        RestingOrder o;
        o.price = std::stod(e[0].get_ref<const nlohmann::json::string_t&>());
        o.size = std::stod(e[1].get_ref<const nlohmann::json::string_t&>());
        o.order_id = parseUuid(e[2].get_ref<const nlohmann::json::string_t&>());
        o.side = side;
        out.push_back(std::move(o));
    }
}

} // namespace

Uuid parseUuid(std::string_view s) {
    Uuid out{};
    int nibbles = 0;
    for (const char c : s) {
        if (c == '-') {
            continue; // dashes are cosmetic separators
        }
        std::uint64_t v;
        if (c >= '0' && c <= '9') {
            v = static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v = static_cast<std::uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v = static_cast<std::uint64_t>(c - 'A' + 10);
        } else {
            throw std::invalid_argument("parseUuid: non-hex character");
        }
        // First 16 hex digits -> high 64 bits, next 16 -> low 64 bits.
        if (nibbles < 16) {
            out.hi = (out.hi << 4) | v;
        } else {
            out.lo = (out.lo << 4) | v;
        }
        ++nibbles;
    }
    if (nibbles != 32) {
        throw std::invalid_argument("parseUuid: expected 32 hex digits");
    }
    return out;
}

L3Snapshot fetchL3Snapshot(const std::string& product) {
    const std::string url = "https://api.exchange.coinbase.com/products/" +
                            product + "/book?level=3";
    // -s: silent progress; -f: fail (empty output + nonzero) on HTTP >= 400.
    const std::string body = runCaptureStdout("curl -sf \"" + url + "\"");
    if (body.empty()) {
        throw std::runtime_error("empty snapshot response for " + product +
                                 " (curl failed or non-2xx)");
    }

    const auto j = nlohmann::json::parse(body);
    L3Snapshot snap;
    snap.sequence = j.at("sequence").get<std::uint64_t>();
    parseSide(j.at("bids"), 'B', snap.bids);
    parseSide(j.at("asks"), 'S', snap.asks);
    return snap;
}

} // namespace babo::feed
