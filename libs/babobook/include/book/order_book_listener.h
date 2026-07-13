//
// Created by Adminstudio on 6/25/2026.
//

#ifndef BABOMATCHINGENGINE_ORDER_BOOK_LISTENER_H
#define BABOMATCHINGENGINE_ORDER_BOOK_LISTENER_H

namespace babo::book {

/// @brief generic listener of order book events
template <class OrderBook >
class OrderBookListener {
public:
  /// @brief callback for change anywhere in order book
  virtual void on_order_book_change(const OrderBook* book) = 0;
};

}

#endif // BABOMATCHINGENGINE_ORDER_BOOK_LISTENER_H
