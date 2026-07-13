//
// Created by hrcol on 26.6.2026..
//

#ifndef BABOMATCHINGENGINE_PRICE_LEVEL_DESCRIPTOR_H
#define BABOMATCHINGENGINE_PRICE_LEVEL_DESCRIPTOR_H

#include "pin_node.h"

#include <cstdint>

namespace babo  {

struct order_loc
{
    pin_node_t* pin_loc;   // the concrete pin_node type (pin_node<kNodeCapacity>)
    uint16_t index;        // slot index within pin_loc

    [[nodiscard]] bool valid() const noexcept { return pin_loc != nullptr; }
    bool operator==(const order_loc&) const = default;
};

enum class color : uint8_t {RED, BLACK};

// Pure metadata for a price level. It does NOT own PIN nodes -- orders live in the tree's
// single global PIN chain. _head/_tail point at this level's first/last orders in that chain
// (its contiguous priority sub-range). The chain layer (narb_tree) maintains them.
struct price_level_descriptor
{
    price_level_descriptor *left, *right, *parent;
    price_level_descriptor *pred, *succ;

    color _color;
    uint64_t _price;
    uint64_t _quantity;   // aggregate open qty resting at this level
    uint32_t _count;      // number of resting orders at this level (for O(SIZE) depth walk)

    order_loc _head;   // highest-priority (best) order at this level
    order_loc _tail;   // lowest-priority (newest) order at this level

    [[nodiscard]] bool empty() const noexcept { return _head.pin_loc == nullptr; }
};

// Process-wide pool for price_level_descriptor objects (Meyers singleton; single-threaded
// engine). Mirrors pin_node_pool(). allocate() value-initialises the descriptor.
inline memory::AllocatorPool<price_level_descriptor>& price_level_descriptor_pool()
{
    static memory::AllocatorPool<price_level_descriptor> pool;
    return pool;
}

}
#endif //BABOMATCHINGENGINE_PRICE_LEVEL_DESCRIPTOR_H
