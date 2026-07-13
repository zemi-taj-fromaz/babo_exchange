//
// Created by Adminstudio on 6/25/2026.
//

#ifndef BABOMATCHINGENGINE_TRADE_LISTENER_H
#define BABOMATCHINGENGINE_TRADE_LISTENER_H

namespace babo::book {

/// @brief listener of trade events.   Implement to build a trade feed.
template <class OrderBook >
class TradeListener {
public:
  /// @brief callback for a trade
  /// @param book the order book of the fill (not defined whether this is before
  ///      or after fill)
  /// @param qty the quantity of this fill
  /// @param cost the cost of this fill (qty * price)
  virtual void on_trade(const OrderBook* book,
                        uint64_t qty,
                        uint64_t cost) = 0;
};

}

#endif // BABOMATCHINGENGINE_TRADE_LISTENER_H
