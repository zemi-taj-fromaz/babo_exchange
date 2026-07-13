#pragma once

#include "egress/depth_snapshot.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace babo::egress {

// Single-writer/single-reader latest-value channel. Unlike an event queue,
// intermediate depth versions may be overwritten: the gateway only needs the
// newest complete snapshot. Every payload field is atomic, while an odd/even
// version guards against observing a mixture of two publications.
class LatestDepthMailbox {
public:
    void publish(const DepthSnapshot& snapshot) noexcept;

    // `lastVersion` is reader-owned. Returns true only when a newer, internally
    // consistent snapshot was copied into `snapshot`.
    [[nodiscard]] bool tryReadNew(std::uint64_t& lastVersion,
                                  DepthSnapshot& snapshot) const noexcept;

private:
    struct AtomicDepthLevel {
        std::atomic<std::uint64_t> price_ticks{0};
        std::atomic<std::uint64_t> qty_lots{0};
        std::atomic<std::uint32_t> order_count{0};
    };

    static void storeLevel(AtomicDepthLevel& destination,
                           const DepthLevelSnapshot& source) noexcept;
    static DepthLevelSnapshot loadLevel(
        const AtomicDepthLevel& source) noexcept;

    // Even = stable publication, odd = writer is updating the payload.
    std::atomic<std::uint64_t> version_{0};
    std::array<AtomicDepthLevel, kPublishedDepthLevels> bids_{};
    std::array<AtomicDepthLevel, kPublishedDepthLevels> asks_{};
};

} // namespace babo::egress
