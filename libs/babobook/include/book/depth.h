#ifndef BABOMATCHINGENGINE_DEPTH_H
#define BABOMATCHINGENGINE_DEPTH_H

#include "depth_constants.h"
#include "depth_level.h"

namespace babo::book {

/// @brief Aggregate top-of-book view: SIZE price levels per side, bids then asks.
///
/// This is a passive SNAPSHOT, not an eagerly-maintained tracker. matching_book
/// rebuilds it on demand (see matching_book::depth()) by walking the narb_tree's
/// top-SIZE price levels best-first -- each price_level_descriptor already carries
/// its aggregate qty (_quantity) and order count (_count), so filling a level is a
/// direct read. Cost: O(SIZE) per QUERY, and *nothing* on the matching hot path.
///
/// This replaces the old liquibook-style tracker that updated on every
/// add/fill/cancel and cached overflow levels in a std::map (a red-black-tree node
/// allocation per deep order). The tree is already the complete ordered structure,
/// so that excess map was pure redundancy: "the next level to promote" is just the
/// next node in the walk. Deriving from the tree is a design liquibook's std::multimap
/// book cannot cheaply match.
template <int SIZE = 5>
class Depth {
public:
  Depth() { clear(); }

  /// @brief reset every level to empty; call before a rebuild
  void clear() {
    for (DepthLevel& l : levels_) l.set(INVALID_LEVEL_PRICE, 0, 0);
  }

  const DepthLevel* bids()           const { return levels_; }
  const DepthLevel* asks()           const { return levels_ + SIZE; }
  const DepthLevel* last_bid_level() const { return levels_ + (SIZE - 1); }
  const DepthLevel* last_ask_level() const { return levels_ + (SIZE * 2 - 1); }
  const DepthLevel* end()            const { return levels_ + (SIZE * 2); }

  DepthLevel* bids()           { return levels_; }
  DepthLevel* asks()           { return levels_ + SIZE; }
  DepthLevel* last_bid_level() { return levels_ + (SIZE - 1); }
  DepthLevel* last_ask_level() { return levels_ + (SIZE * 2 - 1); }

private:
  // [0, SIZE)      bids, best (highest) first
  // [SIZE, 2*SIZE) asks, best (lowest)  first
  DepthLevel levels_[SIZE * 2];
};

}

#endif // BABOMATCHINGENGINE_DEPTH_H
