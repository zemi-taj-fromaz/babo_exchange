#include "egress/latest_depth_mailbox.hpp"

namespace babo::egress {

void LatestDepthMailbox::storeLevel(
    AtomicDepthLevel& destination,
    const DepthLevelSnapshot& source) noexcept {
    destination.price_ticks.store(source.price_ticks,
                                  std::memory_order_seq_cst);
    destination.qty_lots.store(source.qty_lots, std::memory_order_seq_cst);
    destination.order_count.store(source.order_count,
                                  std::memory_order_seq_cst);
}

DepthLevelSnapshot LatestDepthMailbox::loadLevel(
    const AtomicDepthLevel& source) noexcept {
    return DepthLevelSnapshot{
        source.price_ticks.load(std::memory_order_seq_cst),
        source.qty_lots.load(std::memory_order_seq_cst),
        source.order_count.load(std::memory_order_seq_cst),
    };
}

void LatestDepthMailbox::publish(const DepthSnapshot& snapshot) noexcept {
    version_.fetch_add(1, std::memory_order_seq_cst); // stable even -> writing odd

    for (std::size_t i = 0; i < kPublishedDepthLevels; ++i) {
        storeLevel(bids_[i], snapshot.bids[i]);
        storeLevel(asks_[i], snapshot.asks[i]);
    }

    version_.fetch_add(1, std::memory_order_seq_cst); // writing odd -> stable even
}

bool LatestDepthMailbox::tryReadNew(std::uint64_t& lastVersion,
                                    DepthSnapshot& snapshot) const noexcept {
    // A concurrent publication is tiny; bounded retry keeps the gateway loop
    // responsive and it can simply try again on its next iteration.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto before = version_.load(std::memory_order_seq_cst);
        if (before == lastVersion) {
            return false;
        }
        if ((before & 1U) != 0) {
            continue;
        }

        DepthSnapshot candidate;
        for (std::size_t i = 0; i < kPublishedDepthLevels; ++i) {
            candidate.bids[i] = loadLevel(bids_[i]);
            candidate.asks[i] = loadLevel(asks_[i]);
        }

        const auto after = version_.load(std::memory_order_seq_cst);
        if (before == after && (after & 1U) == 0) {
            candidate.sequence = after / 2;
            snapshot = candidate;
            lastVersion = after;
            return true;
        }
    }
    return false;
}

} // namespace babo::egress
