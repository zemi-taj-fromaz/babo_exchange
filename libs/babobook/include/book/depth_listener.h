//
// Created by Adminstudio on 6/25/2026.
//

#ifndef BABOMATCHINGENGINE_DEPTH_LISTENER_H
#define BABOMATCHINGENGINE_DEPTH_LISTENER_H

namespace babo::book {

/// @brief listener of depth events.  Implement to build an aggregate depth
/// feed.
template <class OrderBook >
class DepthListener {
public:
  /// @brief callback for change in tracked aggregated depth
  virtual void on_depth_change(
      const OrderBook* book,
      const typename OrderBook::DepthTracker* depth) = 0;
};

}



#endif // BABOMATCHINGENGINE_DEPTH_LISTENER_H
