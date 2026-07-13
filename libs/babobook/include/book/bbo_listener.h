
#ifndef BABOMATCHINGENGINE_BBO_LISTENER_H
#define BABOMATCHINGENGINE_BBO_LISTENER_H

namespace babo::book {

/// @brief generic listener of top-of-book events
template <class OrderBook>
class BboListener {
public:
  /// @brief callback for top of book change
  virtual void on_bbo_change(
      const OrderBook* book,
      const typename OrderBook::DepthTracker* depth) = 0;
};

}


#endif // BABOMATCHINGENGINE_BBO_LISTENER_H
