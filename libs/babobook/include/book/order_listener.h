//
// Created by Adminstudio on 6/25/2026.
//

#ifndef BABOMATCHINGENGINE_ORDER_LISTENER_H
#define BABOMATCHINGENGINE_ORDER_LISTENER_H

#include <cstdint>

namespace babo::book {

/// @brief one canonical match between an incoming taker and a resting maker
template <typename OrderId>
struct Fill {
  OrderId maker_id;
  OrderId taker_id;
  std::uint64_t qty;
  std::uint64_t price;
};

/// @brief canonical listener for matching-engine domain events
template <typename OrderId>
class OrderListener {
public:
  virtual ~OrderListener() = default;

  /// @brief callback for an order accept
  virtual void on_accept(const OrderId& order) = 0;

  /// @brief callback for an order reject
  virtual void on_reject(const OrderId& order, const char* reason) = 0;

  /// @brief callback for one execution; price is the resting maker's price
  virtual void on_fill(const Fill<OrderId>& fill) = 0;

  /// @brief callback for an order cancellation
  virtual void on_cancel(const OrderId& order) = 0;

  /// @brief callback for an order cancel rejection
  virtual void on_cancel_reject(const OrderId& order, const char* reason) = 0;

  /// @brief callback for an order replace
  /// @param order the replaced order
  /// @param size_delta the change to order quantity
  /// @param new_price the updated order price
  virtual void on_replace(const OrderId& order,
                          const std::int64_t& size_delta,
                          std::uint64_t new_price) = 0;

  /// @brief callback for an order replace rejection
  virtual void on_replace_reject(const OrderId& order, const char* reason) = 0;
};

}

#endif // BABOMATCHINGENGINE_ORDER_LISTENER_H
