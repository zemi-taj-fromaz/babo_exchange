#ifndef BABOMATCHINGENGINE_SIMPLE_ORDER_H
#define BABOMATCHINGENGINE_SIMPLE_ORDER_H


#include <book/types.h>

namespace babo { struct price_level_descriptor; }   // fwd-decl: orders back-point to their level

namespace babo::simple {

enum OrderState {
  os_new,
  os_accepted,
  os_complete,
  os_cancelled,
  os_rejected
};

/// @brief a concrete, non-polymorphic order stored by value in the book's PIN slots.
class SimpleOrder {
public:
  /// @brief default ctor -- produces an empty placeholder order (order_id_ == 0).
  /// Needed so orders can live by value in pin_node's pre-sized slot region;
  /// placeholder slots are never reached because their links stay npos.
  SimpleOrder();

  SimpleOrder(bool is_buy,
              uint32_t price,
              uint32_t qty,
              uint32_t stop_price = 0,
              book::OrderConditions conditions = book::OrderCondition::oc_no_conditions);

  /// @brief test hook: reset the auto-increment id generator so the next
  /// constructed order gets id 1 again. Lets each test case start from a
  /// deterministic id sequence (ids are only required unique per book lifetime).
  static void reset_id_generator() { last_order_id_ = 0; }

  /// @brief get the order's state
  const OrderState& state() const;

  /// @brief is this a limit order? (has a positive price)
  bool is_limit() const;

  /// @brief is this order a buy?
  bool is_buy() const;

  /// @brief get the limit price of this order
  uint32_t price() const;

  uint32_t stop_price()const;

  /// @brief get the quantity of this order
  uint32_t order_qty() const;

  /// @brief get the open quantity of this order (remaining, minus any reserved)
  uint32_t open_qty() const;

  /// @brief tentatively set aside (reserve) quantity during AON deferred-match
  /// planning, without filling it. Lowers the reported open_qty() so the same
  /// quantity isn't double-counted while an AON match is being assembled.
  /// @param reserved amount to add to the reservation (may be negative to release)
  /// @return the remaining available quantity after this reservation
  uint32_t reserve(int32_t reserved);

  /// @brief is there no remaining open quantity in this order?
  bool filled() const;

  /// @brief modify the order's quantity by delta (used by replace).
  /// Mirrors the old OrderTracker::change_qty now that the order tracks itself.
  void change_qty(int32_t delta);

  /// @brief get the filled quantity of this order
  const uint32_t& filled_qty() const;

  /// @brief get the total filled cost of this order
  const uint32_t& filled_cost() const;

  /// @brief notify of a fill of this order
  /// @param fill_qty the number of shares in this fill
  /// @param fill_cost the total amount of this fill
  /// @fill_id the unique identifier of this fill
  void fill(uint32_t fill_qty,
                    uint32_t fill_cost,
                    uint32_t fill_id);

  /// @brief get order conditions as a bit mask
  book::OrderConditions conditions() const;

  /// @brief if no trades should happen until the order
  /// can be filled completely.
  /// Note: one or more trades may be used to fill the order.
  bool all_or_none() const;

  /// @brief After generating as many trades as possible against
  /// orders already on the market, cancel any remaining quantity.
  bool immediate_or_cancel() const;

  /// @brief exchange accepted this order
  void accept();
  /// @brief exchange cancelled this order
  void cancel();

  /// @brief exchange replaced this order
  /// @param size_delta change to the order quantity
  /// @param new_price the new price
  void replace(uint32_t size_delta, uint32_t new_price);

  /// @brief unconditionally modify qty/price for a book replace (keeps id + filled history).
  /// @param size_delta change to the total order quantity (may be negative)
  /// @param new_price the new limit price, or PRICE_UNCHANGED (0) to keep the current price
  void modify(int32_t size_delta, uint64_t new_price);

private:
  OrderState state_;
  bool is_buy_;
  uint32_t order_qty_;
  uint32_t price_;
  uint32_t stop_price_;
  book::OrderConditions conditions_;
  uint32_t filled_qty_;
  uint32_t filled_cost_;
  int32_t reserved_;            // quantity tentatively earmarked during AON planning (not yet filled)
  inline static uint32_t last_order_id_{0};

public:
  uint32_t order_id_;   // non-const so orders are assignable into pin_node slots

  // Back-pointer to the owning price level. Travels with the payload on relocation,
  // so a slot's owning level is always node->at(slot)._level (O(1), no lookup).
  babo::price_level_descriptor* _level = nullptr;
};

}

#endif // BABOMATCHINGENGINE_SIMPLE_ORDER_H
