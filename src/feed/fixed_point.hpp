#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace babo::feed {

inline constexpr unsigned kPriceFractionalDigits = 2;
inline constexpr unsigned kQtyFractionalDigits = 8;

// Parse a non-negative decimal string directly into fixed-point units. This
// deliberately avoids a binary floating-point round trip so feed replay stays
// deterministic. Extra fractional digits are accepted only when they are zero.
inline std::uint64_t parseFixedDecimal(std::string_view text,
                                       unsigned fractionalDigits) {
    if (text.empty()) {
        throw std::invalid_argument("empty decimal");
    }

    std::size_t pos = 0;
    if (text[pos] == '+') {
        ++pos;
    }
    if (pos == text.size() || text[pos] == '-') {
        throw std::invalid_argument("expected non-negative decimal");
    }

    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t whole = 0;
    bool sawDigit = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        const auto digit = static_cast<std::uint64_t>(text[pos] - '0');
        if (whole > (kMax - digit) / 10) {
            throw std::out_of_range("decimal integer part overflows uint64");
        }
        whole = whole * 10 + digit;
        sawDigit = true;
        ++pos;
    }

    std::uint64_t fraction = 0;
    unsigned parsedFractionalDigits = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            const auto digit = static_cast<std::uint64_t>(text[pos] - '0');
            if (parsedFractionalDigits < fractionalDigits) {
                fraction = fraction * 10 + digit;
            } else if (digit != 0) {
                throw std::invalid_argument(
                    "decimal has more precision than the fixed-point scale");
            }
            ++parsedFractionalDigits;
            sawDigit = true;
            ++pos;
        }
    }

    if (!sawDigit || pos != text.size()) {
        throw std::invalid_argument("malformed decimal");
    }

    std::uint64_t scale = 1;
    for (unsigned i = 0; i < fractionalDigits; ++i) {
        if (scale > kMax / 10) {
            throw std::out_of_range("fixed-point scale overflows uint64");
        }
        scale *= 10;
    }
    while (parsedFractionalDigits < fractionalDigits) {
        fraction *= 10;
        ++parsedFractionalDigits;
    }

    if (whole > (kMax - fraction) / scale) {
        throw std::out_of_range("fixed-point decimal overflows uint64");
    }
    return whole * scale + fraction;
}

inline std::uint64_t parsePriceTicks(std::string_view text) {
    return parseFixedDecimal(text, kPriceFractionalDigits);
}

inline std::uint64_t parseQtyLots(std::string_view text) {
    return parseFixedDecimal(text, kQtyFractionalDigits);
}

} // namespace babo::feed
