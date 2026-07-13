//
// Created by Adminstudio on 6/25/2026.
//

#ifndef BABOMATCHINGENGINE_ORDER_LISTENER_H
#define BABOMATCHINGENGINE_ORDER_LISTENER_H

namespace babo::book {

/// @brief generic listener of order events.  Implement to build a full order book feed.
//    Used by common version of OrderBook::process_callback().
template <typename OrderPtr>
class OrderListener {
public:
  /// @brief callback for an order accept
  virtual void on_accept(const OrderPtr& order) = 0;

  /// @brief callback for an order reject
  virtual void on_reject(const OrderPtr& order, const char* reason) = 0;

  /// @brief callback for an order fill
  /// @param order the inbound order
  /// @param matched_order the matched order
  /// @param fill_qty the quantity of this fill
  /// @param fill_cost the cost of this fill (qty * price)
  virtual void on_fill(const OrderPtr& order,
                       const OrderPtr& matched_order,
                       uint64_t fill_qty,
                       uint64_t fill_cost) = 0;

  /// @brief callback for an order cancellation
  virtual void on_cancel(const OrderPtr& order) = 0;

  /// @brief callback for an order cancel rejection
  virtual void on_cancel_reject(const OrderPtr& order, const char* reason) = 0;

  /// @brief callback for an order replace
  /// @param order the replaced order
  /// @param size_delta the change to order quantity
  /// @param new_price the updated order price
  virtual void on_replace(const OrderPtr& order,
                          const int64_t& size_delta,
                          uint64_t new_price) = 0;

  /// @brief callback for an order replace rejection
  virtual void on_replace_reject(const OrderPtr& order, const char* reason) = 0;
};

}

#endif // BABOMATCHINGENGINE_ORDER_LISTENER_H
