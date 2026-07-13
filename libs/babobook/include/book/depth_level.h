//
// Created by Adminstudio on 6/25/2026.
//
#ifndef BABOMATCHINGENGINE_DEPTH_LEVEL_H
#define BABOMATCHINGENGINE_DEPTH_LEVEL_H

#include "depth_constants.h"

#include <stdexcept>

namespace babo::book {

/// @brief a single level of the limit order book aggregated by price
class DepthLevel {
public:
  /// @brief construct
  DepthLevel();

  /// @brief assign
  DepthLevel& operator=(const DepthLevel& rhs);

  const uint64_t& price() const;
  uint32_t order_count() const;
  uint64_t aggregate_qty() const;
  bool is_excess() const { return is_excess_; }

  void init(uint64_t price, bool is_excess);

  /// @brief add an order to the level
  /// @param qty open quantity of the order
  void add_order(uint64_t qty);

  /// @brief increase the quantity of existing orders
  /// @param qty amount to increase the quantity by
  void increase_qty(uint64_t qty);

  /// @brief decrease the quantity of existing orders
  /// @param qty amount to decrease the quantity by
  void decrease_qty(uint64_t qty);

  /// @brief overwrite all values of the level
  /// @param price the level price
  /// @param qty the aggegate quantity
  /// @param order_count the number of orders
  /// @param last_change the last change ID (optional)
  void set(uint64_t price,
           uint64_t qty,
           uint32_t order_count,
           uint32_t last_change = 0);

  /// @brief cancel or fill an order, decrease count and quantity
  /// @param qty the closed quantity
  /// @return true if the level is now empty
  bool close_order(uint64_t qty);

  /// @brief set last changed stamp on this level
  void last_change(uint32_t last_change) { last_change_ = last_change; }

  /// @brief get last change stamp for this level
  uint32_t last_change() const { return last_change_; }

  /// @brief has the level changed since the given stamp?
  /// @param last_published_change the stamp to compare to
  bool changed_since(uint32_t last_published_change) const;

private:
  uint64_t price_;
  uint32_t order_count_;
  uint64_t aggregate_qty_;
  bool is_excess_;
public:
  uint32_t last_change_;
};

inline bool
DepthLevel::changed_since(uint32_t last_published_change) const
{
  return last_change_ > last_published_change;
}

inline DepthLevel::DepthLevel()
  : price_(INVALID_LEVEL_PRICE),
  order_count_(0),
  aggregate_qty_(0),
  is_excess_(false)
{
}

inline
DepthLevel& DepthLevel::operator=(const DepthLevel& rhs)
{
  price_ = rhs.price_;
  order_count_ = rhs.order_count_;
  aggregate_qty_ = rhs.aggregate_qty_;
  if (rhs.price_ != INVALID_LEVEL_PRICE) {
    last_change_ = rhs.last_change_;
  }

  // Do not copy is_excess_

  return *this;
}

inline
const uint64_t&
DepthLevel::price() const
{
  return price_;
}

inline
void
DepthLevel::init(uint64_t price, bool is_excess)
{
  price_ = price;
  order_count_ = 0;
  aggregate_qty_ = 0;
  is_excess_ = is_excess;
}

inline
uint32_t
DepthLevel::order_count() const
{
  return order_count_;
}

inline
uint64_t
DepthLevel::aggregate_qty() const
{
  return aggregate_qty_;
}

inline
void
DepthLevel::add_order(uint64_t qty)
{
  // Increment/increase
  ++order_count_;
  aggregate_qty_ += qty;
}

inline
bool
DepthLevel::close_order(uint64_t qty)
{
  bool empty = false;
  // If this is the last order, reset the level
  if (order_count_ == 0) {
    throw std::runtime_error("DepthLevel::close_order "
      "order count too low");
  } else if (order_count_ == 1) {
    order_count_ = 0;
    aggregate_qty_ = 0;
    empty = true;
    // Else, decrement/decrease
  } else {
    --order_count_;
    if (aggregate_qty_ >= qty) {
      aggregate_qty_ -= qty;
    } else {
      throw std::runtime_error("DepthLevel::close_order "
        "level quantity too low");
    }
  }
  return empty;
}

inline
void
DepthLevel::set(uint64_t price,
  uint64_t qty,
  uint32_t order_count,
  uint32_t last_change)
{
  price_ = price;
  aggregate_qty_ = qty;
  order_count_ = order_count;
  last_change_ = last_change;
  is_excess_ = false;   // snapshot levels come straight from the tree, never "excess"
}

inline
void
DepthLevel::increase_qty(uint64_t qty)
{
  aggregate_qty_ += qty;
}

inline
void
DepthLevel::decrease_qty(uint64_t qty)
{
  aggregate_qty_ -= qty;
}

}

#endif // BABOMATCHINGENGINE_DEPTH_LEVEL_H
