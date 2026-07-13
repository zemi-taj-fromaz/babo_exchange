//
// Created by hrcol on 29.6.2026..
//

#ifndef BABOMATCHINGENGINE_NARB_TREE_H
#define BABOMATCHINGENGINE_NARB_TREE_H

#include "price_level_descriptor.h"

#include <cstdint>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <unordered_map>

namespace babo
{

enum class order_type { BID, ASK };

struct search_result
{
    price_level_descriptor *existing, *pred, *succ;
};

template <order_type type>
class narb_tree
{
public:
    using value_type = std::pair<price_level_descriptor*, simple::SimpleOrder>;

    price_level_descriptor* get_best();

    search_result find_neighbors(std::uint64_t price);
    void neighbor_aware_insert(price_level_descriptor *new_node, price_level_descriptor *pred, price_level_descriptor *succ);

    // Bidirectional iterator over price levels in best-first order.
    // Templated on constness so iterator and const_iterator share one body.
    // Walks the pred/succ threads; direction depends on side (resolved at compile time).
    template <bool Const>
    class iterator_impl
    {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = price_level_descriptor;
        using difference_type   = std::ptrdiff_t;
        using pointer   = std::conditional_t<Const, const price_level_descriptor*, price_level_descriptor*>;
        using reference = std::conditional_t<Const, const price_level_descriptor&, price_level_descriptor&>;

        iterator_impl() noexcept : _node(nullptr) {}
        explicit iterator_impl(pointer n) noexcept : _node(n) {}

        // implicit iterator -> const_iterator conversion (not the reverse).
        // Templated so it is never mistaken for a copy constructor (which would
        // otherwise suppress the implicit copy ctor when Const == false).
        template <bool OtherConst>
            requires (Const && !OtherConst)
        iterator_impl(const iterator_impl<OtherConst>& other) noexcept
            : _node(other._node) {}

        reference operator*()  const noexcept { return *_node; }
        pointer   operator->() const noexcept { return  _node; }

        // ++ moves toward WORSE price (best-first walk):
        // bids high->low via pred, asks low->high via succ.
        iterator_impl& operator++() noexcept
        {
            if constexpr (type == order_type::BID) _node = _node->pred;
            else                                   _node = _node->succ;
            return *this;
        }
        iterator_impl operator++(int) noexcept { iterator_impl t = *this; ++(*this); return t; }

        // -- moves toward BETTER price (the opposite thread).
        iterator_impl& operator--() noexcept
        {
            if constexpr (type == order_type::BID) _node = _node->succ;
            else                                   _node = _node->pred;
            return *this;
        }
        iterator_impl operator--(int) noexcept { iterator_impl t = *this; --(*this); return t; }

        bool operator==(const iterator_impl& o) const noexcept { return _node == o._node; }
        bool operator!=(const iterator_impl& o) const noexcept { return _node != o._node; }

    private:
        template <bool> friend class iterator_impl;  // so the const-conversion ctor can read _node
        pointer _node;
    };

    using iterator       = iterator_impl<false>;
    using const_iterator = iterator_impl<true>;

    // begin() = best price; end() = nullptr sentinel (the worst level's toward-worse
    // thread terminates at nullptr, so walking off the end lands here naturally).
    iterator       begin()        noexcept { return iterator(_best); }
    iterator       end()          noexcept { return iterator(nullptr); }
    const_iterator begin()  const noexcept { return const_iterator(_best); }
    const_iterator end()    const noexcept { return const_iterator(nullptr); }
    const_iterator cbegin() const noexcept { return const_iterator(_best); }
    const_iterator cend()   const noexcept { return const_iterator(nullptr); }


    // Find the price level at `price`, or end() if there is no such level.
    iterator       find(std::uint64_t price)       { return iterator(find_node(price)); }
    const_iterator find(std::uint64_t price) const { return const_iterator(find_node(price)); }

    // O(1) lookup of a resting order by id (nullptr if not in this tree).
    [[nodiscard]] simple::SimpleOrder* find_order(std::uint32_t order_id)
    {
        auto it = _order_index.find(order_id);
        if (it == _order_index.end()) return nullptr;
        return &it->second.pin_loc->at(it->second.index);
    }

    // ---- Order iterator: individual orders in global priority order (best -> worst) ----
    // This is the sweep the matching loop drives: it walks the global PIN chain, jumping
    // across nodes and levels transparently (each ++ is O(1) via global_next).
    template <bool Const>
    class order_iterator_impl
    {
        using tree_ptr = std::conditional_t<Const, const narb_tree*, narb_tree*>;
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = simple::SimpleOrder;
        using difference_type   = std::ptrdiff_t;
        using pointer   = std::conditional_t<Const, const simple::SimpleOrder*, simple::SimpleOrder*>;
        using reference = std::conditional_t<Const, const simple::SimpleOrder&, simple::SimpleOrder&>;

        order_iterator_impl() noexcept : _tree(nullptr), _loc{nullptr, 0} {}
        order_iterator_impl(tree_ptr t, order_loc l) noexcept : _tree(t), _loc(l) {}

        reference operator*()  const noexcept { return _loc.pin_loc->at(_loc.index); }
        pointer   operator->() const noexcept { return &_loc.pin_loc->at(_loc.index); }

        order_iterator_impl& operator++() noexcept { _loc = _tree->global_next(_loc); return *this; }
        order_iterator_impl  operator++(int) noexcept { auto t = *this; ++(*this); return t; }

        [[nodiscard]] order_loc loc() const noexcept { return _loc; }   // for erase / level lookup

        bool operator==(const order_iterator_impl& o) const noexcept { return _loc == o._loc; }
        bool operator!=(const order_iterator_impl& o) const noexcept { return !(_loc == o._loc); }

    private:
        tree_ptr  _tree;
        order_loc _loc;
    };

    using order_iterator       = order_iterator_impl<false>;
    using const_order_iterator = order_iterator_impl<true>;

    order_iterator orders_begin() noexcept
    {
        return _chain_head ? order_iterator{this, {_chain_head, _chain_head->head()}}
                           : order_iterator{this, {nullptr, 0}};
    }
    order_iterator orders_end() noexcept { return order_iterator{this, {nullptr, 0}}; }

    // Insert an order into the book, keyed by `key` (the price the tree orders on).
    //  Branch 1 (O(1) fast path): does it land on, or between, best and second-best?
    //  Branch 2 (general): find_neighbors, then place into an existing level or a new one.
    void insert(simple::SimpleOrder& order, std::uint64_t key);
    // Convenience: key on the order's limit price (the normal resting-book case).
    void insert(simple::SimpleOrder& order) { insert(order, order.price()); }

    // O(1) cancel by order id: locate the slot, unlink it from the PIN chain, and
    // clean up an emptied node (and, if the level empties, the level).
    void erase(std::uint32_t order_id);

    // Debug/testing: verify the red-black invariants (BST order, root black, no red-red,
    // equal black-height). Returns true if the tree is a valid RB tree.
    [[nodiscard]] bool validate_rb() const;


private:
    // Locate the level node at `price` (nullptr if absent). Backs find().
    [[nodiscard]] price_level_descriptor* find_node(std::uint64_t price) const;

    // Allocate + initialise a new price level for the given price.
    price_level_descriptor* make_level(std::uint64_t price);
    // Place an order into the global PIN chain (at this level's tail sub-range) + index it.
    void place_order(price_level_descriptor* level, simple::SimpleOrder& order);

    // --- global PIN chain helpers ---
    // Global priority order: _chain_head (best) ... _chain_tail (worst); within a node,
    // head_->tail_ intra-node links; across nodes, next_node_/prev_node_.
    order_loc chain_first(simple::SimpleOrder&& ord, price_level_descriptor* lvl);           // empty chain
    order_loc chain_insert_head(simple::SimpleOrder&& ord, price_level_descriptor* lvl);      // new global best
    order_loc chain_insert_after(order_loc anchor, simple::SimpleOrder&& ord, price_level_descriptor* lvl);
    [[nodiscard]] order_loc global_next(order_loc loc) const;   // next order in global priority order
    [[nodiscard]] order_loc global_prev(order_loc loc) const;   // previous order

    // Relocation cascade: guarantee X has a free slot by rippling boundary orders toward the
    // tail (reuse a free slot within D_max, else allocate a node at the boundary).
    static constexpr int kMaxCascadeHops = 16;
    void make_room(pin_node_t* X);
    void relocate_tail_to_head(pin_node_t* src, pin_node_t* dst);   // one Push Back hop

    // order id -> resting location, for O(1) cancel.
    std::unordered_map<std::uint32_t, order_loc> _order_index;

    price_level_descriptor* _best = nullptr;
    price_level_descriptor* _root = nullptr;

    // Endpoints of the single global PIN chain for this side.
    pin_node_t* _chain_head = nullptr;   // holds the best (highest-priority) orders
    pin_node_t* _chain_tail = nullptr;   // holds the worst

    std::uint32_t _node_capacity{};

    void insert_rebalance(price_level_descriptor *new_node);

    void right_rotate(price_level_descriptor* node);
    void left_rotate(price_level_descriptor* node);

    // --- level removal (empty level) ---
    void erase_level(price_level_descriptor* z);               // unlink threads/best, RB-delete, recycle
    void rb_delete_node(price_level_descriptor* z);            // CLRS RB-delete (tree structure only)
    void rb_delete_fixup(price_level_descriptor* x, price_level_descriptor* x_parent);
    void transplant(price_level_descriptor* u, price_level_descriptor* v);
    static price_level_descriptor* tree_minimum(price_level_descriptor* n);
    static color color_of(price_level_descriptor* n) noexcept { return n ? n->_color : color::BLACK; }
    static int rb_check(price_level_descriptor* n, std::uint64_t lo, std::uint64_t hi);
};

template <order_type type>
price_level_descriptor* narb_tree<type>::get_best()
{
    return _best;
}

template <order_type type>
price_level_descriptor* narb_tree<type>::find_node(std::uint64_t price) const
{
    price_level_descriptor* curr = _root;
    while (curr)
    {
        if      (price < curr->_price) curr = curr->left;
        else if (price > curr->_price) curr = curr->right;
        else                           return curr;   // exact price match
    }
    return nullptr;   // no level at this price
}

template <order_type type>
search_result narb_tree<type>::find_neighbors(std::uint64_t price)
{
    if (!_root)  return {nullptr, nullptr, nullptr};
    price_level_descriptor* curr = _root;
    while (curr)
    {
        if (price < curr->_price)
        {
            if (!curr->left) return {nullptr, curr->pred, curr};
            curr = curr->left;
        }
        else if (price > curr->_price)
        {
            if (!curr->right) return {nullptr, curr, curr->succ};
            curr = curr->right;
        }
        else return {curr, curr->pred, curr->succ};
    }

    return {nullptr, nullptr, nullptr};
}

template <order_type type>
void narb_tree<type>::left_rotate(price_level_descriptor* node)
{
    // node's right child takes node's place; node descends to the left.
    price_level_descriptor* pivot = node->right;

    node->right = pivot->left;
    if (pivot->left) pivot->left->parent = node;

    pivot->parent = node->parent;
    if (!node->parent)                     _root = pivot;
    else if (node == node->parent->left)   node->parent->left = pivot;
    else                                   node->parent->right = pivot;

    pivot->left = node;
    node->parent = pivot;
}

template <order_type type>
void narb_tree<type>::right_rotate(price_level_descriptor* node)
{
    // Mirror of left_rotate: node's left child takes node's place.
    price_level_descriptor* pivot = node->left;

    node->left = pivot->right;
    if (pivot->right) pivot->right->parent = node;

    pivot->parent = node->parent;
    if (!node->parent)                     _root = pivot;
    else if (node == node->parent->right)  node->parent->right = pivot;
    else                                   node->parent->left = pivot;

    pivot->right = node;
    node->parent = pivot;
}

template <order_type type>
void narb_tree<type>::insert_rebalance(price_level_descriptor *x)
{
    x->_color = color::RED;

    while ( (x != _root) && (x->parent->_color == color::RED) )
    {
       if ( x->parent == x->parent->parent->left )
       {
           /* If x's parent is a left, y is x's right 'uncle' */
            price_level_descriptor* y = x->parent->parent->right;
           if ( y && y->_color == color::RED){
               x->parent->_color = color::BLACK;
               y->_color = color::BLACK;
               x->parent->parent->_color = color::RED;
               x = x->parent->parent;
               }
           else {
               if ( x == x->parent->right ) {
                   x = x->parent;
                   left_rotate(x);
               }

               x->parent->_color = color::BLACK;
               x->parent->parent->_color = color::RED;
               right_rotate(x->parent->parent );
           }
       }
       else {
            // Symmetric case for when parent is a right child
            price_level_descriptor* y = x->parent->parent->left; // Uncle

            if (y && y->_color == color::RED) {
                x->parent->_color = color::BLACK;
                y->_color = color::BLACK;
                x->parent->parent->_color = color::RED;
                x = x->parent->parent;
            } else {
                if (x == x->parent->left) {
                    x = x->parent;
                    right_rotate(x);
                }
                x->parent->_color = color::BLACK;
                x->parent->parent->_color = color::RED;
                left_rotate(x->parent->parent);
            }
       }
    }
    /* Colour the root black */
    _root->_color = color::BLACK;
}


template <order_type type>
void narb_tree<type>::neighbor_aware_insert(price_level_descriptor *new_node, price_level_descriptor *pred, price_level_descriptor *succ)
{
    //check for emty tree

    if (!_root)
    {
        _root = new_node;
        _best = new_node;
    }
    else
    {
        if (pred && !pred->right)
        {
            pred->right = new_node;
            new_node->parent = pred;
        } else
        {
            succ->left = new_node;
            new_node->parent = succ;
        }
    }

    new_node->pred = pred;
    new_node->succ = succ;
    if (pred) pred->succ = new_node;
    if (succ) succ->pred = new_node;

   // 4. Update _best pointer if needed (O(1) price discovery)
    // For ASKs: Best is MIN (no predecessor)
    // For BIDs: Best is MAX (no successor)
    if constexpr(type == order_type::BID)
    {
        if (!succ) _best = new_node;
    }
    else if constexpr(type == order_type::ASK)
    {
        if (!pred) _best = new_node;
    }

    //update best pointer if needed
    //update neighbor links and pin chain
    //insertion fixup
    this->insert_rebalance(new_node);
}

template <order_type type>
void narb_tree<type>::insert(simple::SimpleOrder& order, std::uint64_t price)
{
    // `price` is the ordering key (limit price for the resting book, stop price for stop trees).

    // ---- Branch 1: O(1) fast path against the top of book ----
    if (_best)
    {
        // (a) same price as the best level -> just add the order to it.
        if (price == _best->_price)
        {
            place_order(_best, order);
            return;
        }

        // (a2) more aggressive than best -> new top of book (O(1)).
        // bids: better = higher price ; asks: better = lower price.
        bool better_than_best;
        if constexpr (type == order_type::BID) better_than_best = price > _best->_price;
        else                                   better_than_best = price < _best->_price;
        if (better_than_best)
        {
            price_level_descriptor* level = make_level(price);
            // Splice into the tree FIRST so pred/succ threads are set before place_order
            // (which reads them to find the order's slot in the global chain).
            // The new level sits immediately beyond the old best on the aggressive side.
            if constexpr (type == order_type::BID) neighbor_aware_insert(level, /*pred=*/_best, /*succ=*/nullptr);
            else                                   neighbor_aware_insert(level, /*pred=*/nullptr, /*succ=*/_best);
            place_order(level, order);
            return;
        }

        // The second-best level is one step toward worse price.
        // bids: worse = lower price = pred ; asks: worse = higher price = succ.
        price_level_descriptor* next =
            (type == order_type::BID) ? _best->pred : _best->succ;

        // (a3) best has no worse neighbour -> it is the ONLY level, and (having passed
        // (a)/(a2)) price is worse than best. Splice the new level straight onto best's
        // worse side. O(1), no traversal. Mirrors find_neighbors' single-node result.
        if (!next)
        {
            price_level_descriptor* level = make_level(price);
            if constexpr (type == order_type::BID) neighbor_aware_insert(level, /*pred=*/nullptr, /*succ=*/_best);
            else                                   neighbor_aware_insert(level, /*pred=*/_best,    /*succ=*/nullptr);
            place_order(level, order);
            return;
        }

        // (b) same price as the second-best level.
        if (price == next->_price)
        {
            place_order(next, order);
            return;
        }

        // (c) strictly between best and second-best -> new level in the gap.
        // find_neighbors/neighbor_aware_insert use raw price (pred = lower price,
        // succ = higher price), so just pass the numeric lower/higher of the two.
        price_level_descriptor* lo = (_best->_price < next->_price) ? _best : next;
        price_level_descriptor* hi = (_best->_price < next->_price) ? next : _best;
        if (price > lo->_price && price < hi->_price)
        {
            price_level_descriptor* level = make_level(price);
            neighbor_aware_insert(level, /*pred=*/lo, /*succ=*/hi);
            place_order(level, order);
            return;
        }
    }

    // ---- Branch 2: general path ----
    search_result sr = find_neighbors(price);
    if (sr.existing)
    {
        // Price already has a level: drop the order into it.
        place_order(sr.existing, order);
    }
    else
    {
        // New price: splice into the tree first (sets pred/succ), then place the order.
        price_level_descriptor* level = make_level(price);
        neighbor_aware_insert(level, sr.pred, sr.succ);
        place_order(level, order);
    }
}

template <order_type type>
price_level_descriptor* narb_tree<type>::make_level(std::uint64_t price)
{
    price_level_descriptor* level = price_level_descriptor_pool().allocate();  // value-initialised
    level->left = level->right = level->parent = nullptr;
    level->pred = level->succ = nullptr;
    level->_color = color::RED;   // neighbor_aware_insert -> insert_rebalance fixes colours
    level->_price = price;
    level->_quantity = 0;
    level->_count = 0;
    level->_head = {nullptr, 0};  // empty PIN chain; first insert allocates the node
    level->_tail = {nullptr, 0};
    return level;
}

template <order_type type>
void narb_tree<type>::place_order(price_level_descriptor* level, simple::SimpleOrder& order)
{
    const std::uint32_t id  = order.order_id_;    // read before the order is moved into a slot
    const std::uint64_t  qty = order.open_qty();
    const bool level_was_empty = level->empty();

    order_loc loc;
    if (!_chain_head)
    {
        loc = chain_first(std::move(order), level);                          // first order on this side
    }
    else if (!level_was_empty)
    {
        loc = chain_insert_after(level->_tail, std::move(order), level);      // append at this level's tail
    }
    else
    {
        // New level: insert right after the block of the level immediately BETTER (higher
        // priority) than it. pred/succ are numeric (pred=lower price, succ=higher); global
        // priority is descending price for bids, ascending for asks -- so "better" flips.
        price_level_descriptor* better = (type == order_type::BID) ? level->succ : level->pred;
        if (better)
            loc = chain_insert_after(better->_tail, std::move(order), level); // after the better level's block
        else
            loc = chain_insert_head(std::move(order), level);                // new global best
    }

    if (level_was_empty) level->_head = loc;
    level->_tail = loc;
    _order_index[id] = loc;
    level->_quantity += qty;
    ++level->_count;                 // one more resting order at this level (depth walk)
}

template <order_type type>
order_loc narb_tree<type>::chain_first(simple::SimpleOrder&& ord, price_level_descriptor* lvl)
{
    pin_node_t* node = pin_node_pool().allocate();
    _chain_head = _chain_tail = node;
    std::uint16_t s = node->append(std::move(ord));
    node->at(s)._level = lvl;
    return {node, s};
}

template <order_type type>
order_loc narb_tree<type>::chain_insert_head(simple::SimpleOrder&& ord, price_level_descriptor* lvl)
{
    pin_node_t* h = _chain_head;
    if (!h->full())
    {
        std::uint16_t s = h->prepend(std::move(ord));   // new intra-node head = new global head
        h->at(s)._level = lvl;
        return {h, s};
    }
    // Head node full: prepend a fresh node at the head of the chain.
    pin_node_t* node = pin_node_pool().allocate();
    node->set_next_node(h);
    h->set_prev_node(node);
    _chain_head = node;
    std::uint16_t s = node->append(std::move(ord));
    node->at(s)._level = lvl;
    return {node, s};
}

template <order_type type>
order_loc narb_tree<type>::chain_insert_after(order_loc anchor, simple::SimpleOrder&& ord,
                                              price_level_descriptor* lvl)
{
    pin_node_t* nX = anchor.pin_loc;

    // Fast path: room in the anchor node -> splice in place.
    if (!nX->full())
    {
        std::uint16_t s = nX->insert_after(anchor.index, std::move(ord));
        nX->at(s)._level = lvl;
        return {nX, s};
    }

    // Node full, Case B: anchor is the node's tail -> order belongs at the nX/next boundary.
    if (anchor.index == nX->tail())
    {
        pin_node_t* next = nX->next_node();
        if (!next)
        {
            // nX is the global tail -> extend the chain with a fresh tail node.
            pin_node_t* node = pin_node_pool().allocate();
            node->set_prev_node(nX);
            nX->set_next_node(node);
            _chain_tail = node;
            std::uint16_t s = node->append(std::move(ord));
            node->at(s)._level = lvl;
            return {node, s};
        }
        if (next->full()) make_room(next);                    // free a slot at next's head end
        std::uint16_t s = next->prepend(std::move(ord));      // new head of next = right after nX's tail
        next->at(s)._level = lvl;
        return {next, s};
    }

    // Node full, Case A: anchor is interior to nX -> order belongs strictly inside nX.
    // Free a slot in nX via a Push Back cascade (the anchor is safe -- we evict nX's tail).
    make_room(nX);
    std::uint16_t s = nX->insert_after(anchor.index, std::move(ord));
    nX->at(s)._level = lvl;
    return {nX, s};
}

template <order_type type>
void narb_tree<type>::make_room(pin_node_t* X)
{
    // Walk toward the tail (up to D_max) for a node with a free slot; else allocate one.
    pin_node_t* path[kMaxCascadeHops + 2];
    int len = 0;
    path[len++] = X;
    pin_node_t* n = X;
    int hops = 0;
    while (n->full() && hops < kMaxCascadeHops)
    {
        pin_node_t* nx = n->next_node();
        if (!nx) break;                 // reached the chain tail
        n = nx;
        path[len++] = n;
        ++hops;
    }
    if (n->full())
    {
        // No free slot within D_max (or all-full to the chain tail) -> allocate at the boundary.
        pin_node_t* m     = pin_node_pool().allocate();
        pin_node_t* after = n->next_node();
        m->set_prev_node(n);
        m->set_next_node(after);
        n->set_next_node(m);
        if (after) after->set_prev_node(m); else _chain_tail = m;
        path[len++] = m;
    }
    // Ripple from the far end inward: move each node's tail into the next node's head.
    // Frees exactly one slot in X (path[0]); every dst has room when we move into it.
    for (int i = len - 2; i >= 0; --i)
        relocate_tail_to_head(path[i], path[i + 1]);
}

template <order_type type>
void narb_tree<type>::relocate_tail_to_head(pin_node_t* src, pin_node_t* dst)
{
    const std::uint16_t s_old = src->tail();
    const order_loc old_loc{src, s_old};
    price_level_descriptor* L = src->at(s_old)._level;      // embedded back-ptr

    simple::SimpleOrder moved = std::move(src->at(s_old));  // carry payload (+_level) out
    src->erase(s_old);                                      // free src's tail slot
    const std::uint16_t s_new = dst->prepend(std::move(moved));   // becomes dst's head (same global pos)
    const order_loc new_loc{dst, s_new};

    _order_index[dst->at(s_new).order_id_] = new_loc;       // (1) fix id -> loc
    if (L->_tail == old_loc) L->_tail = new_loc;            // (2) fix owning level's tail
    if (L->_head == old_loc) L->_head = new_loc;            // (3) fix owning level's head
}

template <order_type type>
order_loc narb_tree<type>::global_next(order_loc loc) const
{
    const std::uint16_t n = loc.pin_loc->next(loc.index);
    if (n != pin_node_t::npos) return {loc.pin_loc, n};
    if (pin_node_t* nn = loc.pin_loc->next_node()) return {nn, nn->head()};
    return {nullptr, 0};
}

template <order_type type>
order_loc narb_tree<type>::global_prev(order_loc loc) const
{
    const std::uint16_t p = loc.pin_loc->prev(loc.index);
    if (p != pin_node_t::npos) return {loc.pin_loc, p};
    if (pin_node_t* pn = loc.pin_loc->prev_node()) return {pn, pn->tail()};
    return {nullptr, 0};
}

template <order_type type>
void narb_tree<type>::erase(std::uint32_t order_id)
{
    auto it = _order_index.find(order_id);
    if (it == _order_index.end()) return;   // unknown / already cancelled

    const order_loc loc = it->second;
    pin_node_t* node = loc.pin_loc;
    price_level_descriptor* level = node->at(loc.index)._level;   // embedded back-pointer
    _order_index.erase(it);

    level->_quantity -= node->at(loc.index).open_qty();
    --level->_count;                 // one fewer resting order at this level (depth walk)

    const bool was_head = (level->_head.pin_loc == node && level->_head.index == loc.index);
    const bool was_tail = (level->_tail.pin_loc == node && level->_tail.index == loc.index);

    bool level_empty = false;
    if (was_head && was_tail)
    {
        level->_head = {nullptr, 0};    // was the level's only order
        level->_tail = {nullptr, 0};
        level_empty = true;
    }
    else
    {
        // Recompute head/tail BEFORE unlinking (global_next/prev read the live links).
        if (was_head) level->_head = global_next(loc);
        if (was_tail) level->_tail = global_prev(loc);
    }

    node->erase(loc.index);   // unlink from intra-node priority list + recycle slot

    if (node->empty())
    {
        // Drop the emptied node out of the global chain and return it to the pool.
        pin_node_t* prev = node->prev_node();
        pin_node_t* next = node->next_node();
        if (prev) prev->set_next_node(next); else _chain_head = next;
        if (next) next->set_prev_node(prev); else _chain_tail = prev;
        pin_node_pool().release(node);
    }

    if (level_empty)
        erase_level(level);   // remove the empty level from the tree + recycle its descriptor
}

// ---- level removal: pred/succ threads + best, then RB-delete, then recycle ----

template <order_type type>
void narb_tree<type>::erase_level(price_level_descriptor* z)
{
    price_level_descriptor* p = z->pred;
    price_level_descriptor* s = z->succ;

    // Advance _best to the next-best level (bids: worse=lower=pred ; asks: worse=higher=succ).
    if (z == _best)
        _best = (type == order_type::BID) ? p : s;

    // Unlink from the in-order (price) thread list.
    if (p) p->succ = s;
    if (s) s->pred = p;

    rb_delete_node(z);                           // remove from the RB tree (structure only)
    price_level_descriptor_pool().release(z);    // return the descriptor to its pool
}

template <order_type type>
price_level_descriptor* narb_tree<type>::tree_minimum(price_level_descriptor* n)
{
    while (n->left) n = n->left;
    return n;
}

template <order_type type>
void narb_tree<type>::transplant(price_level_descriptor* u, price_level_descriptor* v)
{
    if (!u->parent)               _root = v;
    else if (u == u->parent->left) u->parent->left = v;
    else                           u->parent->right = v;
    if (v) v->parent = u->parent;
}

template <order_type type>
void narb_tree<type>::rb_delete_node(price_level_descriptor* z)
{
    price_level_descriptor* y = z;
    color y_original = y->_color;
    price_level_descriptor* x = nullptr;
    price_level_descriptor* x_parent = nullptr;   // tracked separately (x may be null)

    if (!z->left)
    {
        x = z->right;
        x_parent = z->parent;
        transplant(z, z->right);
    }
    else if (!z->right)
    {
        x = z->left;
        x_parent = z->parent;
        transplant(z, z->left);
    }
    else
    {
        y = tree_minimum(z->right);   // in-order successor (== z->succ)
        y_original = y->_color;
        x = y->right;
        if (y->parent == z)
        {
            x_parent = y;             // x may be null; its parent is conceptually y
        }
        else
        {
            x_parent = y->parent;
            transplant(y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        transplant(z, y);
        y->left = z->left;
        y->left->parent = y;
        y->_color = z->_color;
    }

    if (y_original == color::BLACK)
        rb_delete_fixup(x, x_parent);
}

template <order_type type>
void narb_tree<type>::rb_delete_fixup(price_level_descriptor* x, price_level_descriptor* x_parent)
{
    while (x != _root && color_of(x) == color::BLACK)
    {
        if (x == x_parent->left)
        {
            price_level_descriptor* w = x_parent->right;              // sibling (non-null by RB invariant)
            if (color_of(w) == color::RED)
            {
                w->_color = color::BLACK;
                x_parent->_color = color::RED;
                left_rotate(x_parent);
                w = x_parent->right;
            }
            if (color_of(w->left) == color::BLACK && color_of(w->right) == color::BLACK)
            {
                w->_color = color::RED;
                x = x_parent;
                x_parent = x->parent;
            }
            else
            {
                if (color_of(w->right) == color::BLACK)
                {
                    if (w->left) w->left->_color = color::BLACK;
                    w->_color = color::RED;
                    right_rotate(w);
                    w = x_parent->right;
                }
                w->_color = x_parent->_color;
                x_parent->_color = color::BLACK;
                if (w->right) w->right->_color = color::BLACK;
                left_rotate(x_parent);
                x = _root;               // done
                x_parent = nullptr;
            }
        }
        else
        {
            price_level_descriptor* w = x_parent->left;
            if (color_of(w) == color::RED)
            {
                w->_color = color::BLACK;
                x_parent->_color = color::RED;
                right_rotate(x_parent);
                w = x_parent->left;
            }
            if (color_of(w->right) == color::BLACK && color_of(w->left) == color::BLACK)
            {
                w->_color = color::RED;
                x = x_parent;
                x_parent = x->parent;
            }
            else
            {
                if (color_of(w->left) == color::BLACK)
                {
                    if (w->right) w->right->_color = color::BLACK;
                    w->_color = color::RED;
                    left_rotate(w);
                    w = x_parent->left;
                }
                w->_color = x_parent->_color;
                x_parent->_color = color::BLACK;
                if (w->left) w->left->_color = color::BLACK;
                right_rotate(x_parent);
                x = _root;
                x_parent = nullptr;
            }
        }
    }
    if (x) x->_color = color::BLACK;
}

// ---- RB invariant checker (debug/testing) ----
// Returns the black-height of the subtree, or -1 if any invariant is violated.
template <order_type type>
int narb_tree<type>::rb_check(price_level_descriptor* n, std::uint64_t lo, std::uint64_t hi)
{
    if (!n) return 1;   // null leaves are black
    if (n->_price <= lo || n->_price >= hi) return -1;                 // BST order (distinct prices)
    if (n->_color == color::RED &&
        (color_of(n->left) == color::RED || color_of(n->right) == color::RED))
        return -1;                                                    // no red-red
    const int l = rb_check(n->left,  lo, n->_price);
    const int r = rb_check(n->right, n->_price, hi);
    if (l == -1 || r == -1 || l != r) return -1;                      // equal black-height
    return l + (n->_color == color::BLACK ? 1 : 0);
}

template <order_type type>
bool narb_tree<type>::validate_rb() const
{
    if (!_root) return true;
    if (_root->_color != color::BLACK) return false;
    return rb_check(_root, 0, ~static_cast<std::uint64_t>(0)) != -1;
}

}

#endif //BABOMATCHINGENGINE_NARB_TREE_H
