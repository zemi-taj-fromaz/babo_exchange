// --- babo_book (PLACEHOLDER) ------------------------------------------------
// Stub implementation so everything links. These do nothing yet -- the bench
// numbers for babo are meaningless until you implement real matching here.
//
// Day-1 plan (from building_diary.md): start from liquibook's two-multimap
// design, then try the cache-aware order-book layout from arxiv 2606.01183.
#include "simple/simple_order.h"

#include <stdexcept>

namespace babo::simple {

SimpleOrder::SimpleOrder()
: state_(os_new),
  is_buy_(false),
  order_qty_(0),
  price_(0),
  stop_price_(0),
  conditions_(0),
  filled_qty_(0),
  filled_cost_(0),
  reserved_(0),
  order_id_(0)   // 0 = "no order" sentinel; real ids start at 1
{
}

SimpleOrder::SimpleOrder(
  bool is_buy,
  uint64_t price,
  uint64_t qty,
  uint64_t stop_price,
  book::OrderConditions conditions)
: state_(os_new),
  is_buy_(is_buy),
  order_qty_(qty),
  price_(price),
  stop_price_(stop_price),
  conditions_(conditions),
  filled_qty_(0),
  filled_cost_(0),
  reserved_(0),
  order_id_(++last_order_id_)
{
}

const OrderState&
SimpleOrder::state() const
{
  return state_;
}

bool
SimpleOrder::is_limit() const
{
  return price() > 0;
}

bool
SimpleOrder::is_buy() const
{
  return is_buy_;
}

uint64_t
SimpleOrder::price() const
{
  return price_;
}

uint64_t
SimpleOrder::stop_price() const
{
  return stop_price_;
}

book::OrderConditions
SimpleOrder::conditions() const
{
  return conditions_;
}

bool
SimpleOrder::all_or_none() const
{
  return (conditions_ & book::OrderCondition::oc_all_or_none) != 0;
}

bool
SimpleOrder::immediate_or_cancel() const
{
  return (conditions_ & book::OrderCondition::oc_immediate_or_cancel) != 0;
}
uint64_t
SimpleOrder::order_qty() const
{
  return order_qty_;
}

uint64_t
SimpleOrder::open_qty() const
{
  // Remaining quantity, minus any tentatively reserved amount.
  // If not completely filled, calculate
  if (filled_qty_ < order_qty_) {
    uint64_t remaining = order_qty_ - filled_qty_;
    // Subtract the reservation, guarding against going below zero.
    if (reserved_ > 0 && static_cast<uint64_t>(reserved_) < remaining) {
      return remaining - static_cast<uint64_t>(reserved_);
    }
    return (reserved_ <= 0) ? remaining : 0;
  // Else prevent accidental overflow
  } else {
    return 0;
  }
}

uint64_t
SimpleOrder::reserve(int64_t reserved)
{
  reserved_ += reserved;
  return open_qty();
}

bool
SimpleOrder::filled() const
{
  return filled_qty_ >= order_qty_;
}

void
SimpleOrder::change_qty(int64_t delta)
{
  if (delta < 0 && static_cast<int64_t>(open_qty()) < -delta) {
    throw std::runtime_error("Replace size reduction larger than open quantity");
  }
  order_qty_ += delta;
}

const uint64_t&
SimpleOrder::filled_qty() const
{
  return filled_qty_;
}

const uint64_t&
SimpleOrder::filled_cost() const
{
  return filled_cost_;
}

void
SimpleOrder::fill(uint64_t fill_qty,
                  uint64_t fill_cost,
                  uint32_t /*fill_id*/)
{
  filled_qty_ += fill_qty;
  filled_cost_ += fill_cost;
  if (!open_qty()) {
    state_ = os_complete;
  }
}

void
SimpleOrder::accept()
{
  if (os_new == state_) {
    state_ = os_accepted;
  }
}

void
SimpleOrder::cancel()
{
  if (os_complete != state_) {
    state_ = os_cancelled;
  }
}

void
SimpleOrder::replace(uint64_t size_delta, uint64_t new_price)
{
  if (os_accepted == state_) {
    order_qty_ += size_delta;
    price_ = new_price;
  }
}

void
SimpleOrder::modify(int64_t size_delta, uint64_t new_price)
{
  order_qty_ = static_cast<uint64_t>(static_cast<int64_t>(order_qty_) + size_delta);
  if (new_price != 0 /* PRICE_UNCHANGED */) {
    price_ = new_price;
  }
}

}
