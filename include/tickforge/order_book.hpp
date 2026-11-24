// Price-time priority limit order book and matching engine.
//
// Design (what makes it fast):
//  * Prices are integer ticks inside a fixed range, so a price level is found
//    by array index, not by searching a tree.
//  * Orders live in one preallocated pool and are linked into their level's
//    FIFO queue with indices (an intrusive doubly linked list). Adding,
//    cancelling and filling an order are O(1) and allocate no memory once the
//    pool has warmed up.
//  * An order id encodes its pool slot plus a generation counter, so lookups
//    need no hash map and a stale id (an order that already filled or was
//    cancelled) is detected instead of touching a reused slot.
//  * A two-level bitmap finds the next non-empty level when the best level
//    empties.
//
// Semantics:
//  * Limit orders (GTC rests the remainder, IOC drops it) and market orders.
//  * Fills happen at the resting (maker) order's price, best price first, and
//    oldest order first within a price (price-time priority).
//  * reduce() lowers an order's quantity and keeps its queue position;
//    changing the price means cancel + new order (it loses priority), which is
//    how real exchanges behave.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "tickforge/level_bitmap.hpp"

namespace tf {

using Price = std::int32_t;
using Qty = std::int64_t;
using OrderId = std::uint64_t;
using Owner = std::uint32_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class Tif : std::uint8_t { GTC = 0, IOC = 1 };

inline constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }
inline constexpr OrderId kNoOrder = 0;

struct Fill {
  OrderId maker_id;
  OrderId taker_id;
  Owner maker_owner;
  Owner taker_owner;
  Price price;
  Qty qty;
  Side taker_side;
};

struct AddResult {
  OrderId id = kNoOrder;  // valid (live) only if resting > 0
  Qty filled = 0;
  Qty resting = 0;
  bool rejected = false;
};

struct LevelView {
  Price price;
  Qty qty;
  std::uint32_t orders;
};

class OrderBook {
 public:
  OrderBook(Price min_price, Price max_price, std::size_t reserve_orders = 1 << 16)
      : min_price_(min_price),
        max_price_(max_price),
        n_levels_(checked_levels(min_price, max_price)),
        bid_levels_(n_levels_),
        ask_levels_(n_levels_),
        bid_bits_(n_levels_),
        ask_bits_(n_levels_) {
    orders_.reserve(reserve_orders);
    free_.reserve(reserve_orders);
    fills_.reserve(64);
  }

  Price min_price() const { return min_price_; }
  Price max_price() const { return max_price_; }

  // ---- order entry -------------------------------------------------------

  AddResult add_limit(Side side, Price price, Qty qty, Owner owner = 0, Tif tif = Tif::GTC) {
    fills_.clear();
    AddResult r;
    if (qty <= 0 || price < min_price_ || price > max_price_) {
      r.rejected = true;
      return r;
    }
    const std::uint32_t slot = alloc();
    const OrderId taker_id = make_id(slot);
    const std::size_t limit_idx = idx(price);
    Qty remaining = qty;

    if (side == Side::Buy) {
      while (remaining > 0 && best_ask_ != LevelBitmap::npos && best_ask_ <= limit_idx) {
        remaining = match_level(ask_levels_[best_ask_], best_ask_, remaining, taker_id, owner, side);
        if (ask_levels_[best_ask_].count == 0) {
          ask_bits_.clear(best_ask_);
          best_ask_ = ask_bits_.next(best_ask_);
        }
      }
    } else {
      while (remaining > 0 && best_bid_ != LevelBitmap::npos && best_bid_ >= limit_idx) {
        remaining = match_level(bid_levels_[best_bid_], best_bid_, remaining, taker_id, owner, side);
        if (bid_levels_[best_bid_].count == 0) {
          bid_bits_.clear(best_bid_);
          best_bid_ = best_bid_ == 0 ? LevelBitmap::npos : bid_bits_.prev(best_bid_ - 1);
        }
      }
    }

    r.filled = qty - remaining;
    if (remaining > 0 && tif == Tif::GTC) {
      Order& o = orders_[slot];
      o.qty = remaining;
      o.price = price;
      o.owner = owner;
      o.side = side;
      o.live = true;
      append(side, limit_idx, slot);
      ++live_;
      r.id = taker_id;
      r.resting = remaining;
    } else {
      release(slot);
    }
    return r;
  }

  AddResult add_market(Side side, Qty qty, Owner owner = 0) {
    return add_limit(side, side == Side::Buy ? max_price_ : min_price_, qty, owner, Tif::IOC);
  }

  bool cancel(OrderId id) {
    const std::uint32_t slot = live_slot(id);
    if (slot == kNil) return false;
    Order& o = orders_[slot];
    unlink(o.side, idx(o.price), slot);
    o.live = false;
    --live_;
    release(slot);
    return true;
  }

  // Lower an order's quantity without losing its place in the queue.
  bool reduce(OrderId id, Qty new_qty) {
    const std::uint32_t slot = live_slot(id);
    if (slot == kNil) return false;
    Order& o = orders_[slot];
    if (new_qty <= 0) return cancel(id);
    if (new_qty >= o.qty) return false;
    level(o.side, idx(o.price)).total -= (o.qty - new_qty);
    o.qty = new_qty;
    return true;
  }

  // ---- queries -----------------------------------------------------------

  bool has_bid() const { return best_bid_ != LevelBitmap::npos; }
  bool has_ask() const { return best_ask_ != LevelBitmap::npos; }
  Price best_bid() const { return price_of(best_bid_); }  // only if has_bid()
  Price best_ask() const { return price_of(best_ask_); }  // only if has_ask()

  Qty best_bid_qty() const { return has_bid() ? bid_levels_[best_bid_].total : 0; }
  Qty best_ask_qty() const { return has_ask() ? ask_levels_[best_ask_].total : 0; }

  Qty depth(Side side, Price price) const {
    if (price < min_price_ || price > max_price_) return 0;
    return side == Side::Buy ? bid_levels_[idx(price)].total : ask_levels_[idx(price)].total;
  }

  bool is_live(OrderId id) const { return live_slot(id) != kNil; }

  Qty order_qty(OrderId id) const {
    const std::uint32_t slot = live_slot(id);
    return slot == kNil ? 0 : orders_[slot].qty;
  }

  Price order_price(OrderId id) const {
    const std::uint32_t slot = live_slot(id);
    if (slot == kNil) throw std::out_of_range("order is not live");
    return orders_[slot].price;
  }

  // Quantity resting ahead of this order at its price (its queue position).
  Qty queue_ahead(OrderId id) const {
    const std::uint32_t slot = live_slot(id);
    if (slot == kNil) return -1;
    Qty ahead = 0;
    for (std::uint32_t s = orders_[slot].prev; s != kNil; s = orders_[s].prev) ahead += orders_[s].qty;
    return ahead;
  }

  std::size_t live_orders() const { return live_; }

  // Fills produced by the most recent add_limit / add_market call.
  const std::vector<Fill>& fills() const { return fills_; }

  // Top n levels on one side, best first.
  std::vector<LevelView> levels(Side side, std::size_t n) const {
    std::vector<LevelView> out;
    std::size_t i = side == Side::Buy ? best_bid_ : best_ask_;
    while (i != LevelBitmap::npos && out.size() < n) {
      const Level& lv = side == Side::Buy ? bid_levels_[i] : ask_levels_[i];
      out.push_back({price_of(i), lv.total, lv.count});
      if (side == Side::Buy) {
        i = i == 0 ? LevelBitmap::npos : bid_bits_.prev(i - 1);
      } else {
        i = ask_bits_.next(i + 1);
      }
    }
    return out;
  }

 private:
  static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

  struct Order {
    Qty qty = 0;
    Price price = 0;
    std::uint32_t prev = kNil;
    std::uint32_t next = kNil;
    std::uint32_t gen = 1;
    Owner owner = 0;
    Side side = Side::Buy;
    bool live = false;
  };

  struct Level {
    std::uint32_t head = kNil;
    std::uint32_t tail = kNil;
    Qty total = 0;
    std::uint32_t count = 0;
  };

  static std::size_t checked_levels(Price lo, Price hi) {
    if (hi < lo) throw std::invalid_argument("max_price < min_price");
    return static_cast<std::size_t>(static_cast<std::int64_t>(hi) - lo + 1);
  }

  std::size_t idx(Price p) const { return static_cast<std::size_t>(p - min_price_); }
  Price price_of(std::size_t i) const { return static_cast<Price>(min_price_ + static_cast<std::int64_t>(i)); }

  OrderId make_id(std::uint32_t slot) const {
    return (static_cast<OrderId>(orders_[slot].gen) << 32) | slot;
  }

  std::uint32_t live_slot(OrderId id) const {
    const std::uint32_t slot = static_cast<std::uint32_t>(id & 0xFFFFFFFFu);
    const std::uint32_t gen = static_cast<std::uint32_t>(id >> 32);
    if (id == kNoOrder || slot >= orders_.size()) return kNil;
    const Order& o = orders_[slot];
    return (o.live && o.gen == gen) ? slot : kNil;
  }

  std::uint32_t alloc() {
    if (!free_.empty()) {
      const std::uint32_t s = free_.back();
      free_.pop_back();
      return s;
    }
    orders_.emplace_back();
    return static_cast<std::uint32_t>(orders_.size() - 1);
  }

  void release(std::uint32_t slot) {
    Order& o = orders_[slot];
    o.live = false;
    o.prev = o.next = kNil;
    ++o.gen;
    if (o.gen == 0) o.gen = 1;  // keep ids non-zero after wrap-around
    free_.push_back(slot);
  }

  Level& level(Side side, std::size_t i) { return side == Side::Buy ? bid_levels_[i] : ask_levels_[i]; }

  void append(Side side, std::size_t i, std::uint32_t slot) {
    Level& lv = level(side, i);
    Order& o = orders_[slot];
    o.prev = lv.tail;
    o.next = kNil;
    if (lv.tail != kNil) {
      orders_[lv.tail].next = slot;
    } else {
      lv.head = slot;
    }
    lv.tail = slot;
    lv.total += o.qty;
    ++lv.count;
    if (lv.count == 1) {
      if (side == Side::Buy) {
        bid_bits_.set(i);
        if (best_bid_ == LevelBitmap::npos || i > best_bid_) best_bid_ = i;
      } else {
        ask_bits_.set(i);
        if (best_ask_ == LevelBitmap::npos || i < best_ask_) best_ask_ = i;
      }
    }
  }

  void unlink(Side side, std::size_t i, std::uint32_t slot) {
    Level& lv = level(side, i);
    Order& o = orders_[slot];
    if (o.prev != kNil) orders_[o.prev].next = o.next; else lv.head = o.next;
    if (o.next != kNil) orders_[o.next].prev = o.prev; else lv.tail = o.prev;
    lv.total -= o.qty;
    --lv.count;
    if (lv.count == 0) {
      if (side == Side::Buy) {
        bid_bits_.clear(i);
        if (i == best_bid_) best_bid_ = i == 0 ? LevelBitmap::npos : bid_bits_.prev(i - 1);
      } else {
        ask_bits_.clear(i);
        if (i == best_ask_) best_ask_ = ask_bits_.next(i + 1);
      }
    }
  }

  // Fill against one level, oldest order first. Returns the unfilled quantity.
  Qty match_level(Level& lv, std::size_t i, Qty remaining, OrderId taker_id, Owner taker_owner, Side taker_side) {
    const Price px = price_of(i);
    while (remaining > 0 && lv.head != kNil) {
      const std::uint32_t s = lv.head;
      Order& o = orders_[s];
      const Qty take = std::min(remaining, o.qty);
      fills_.push_back(Fill{make_id(s), taker_id, o.owner, taker_owner, px, take, taker_side});
      o.qty -= take;
      lv.total -= take;
      remaining -= take;
      if (o.qty == 0) {
        lv.head = o.next;
        if (lv.head != kNil) orders_[lv.head].prev = kNil; else lv.tail = kNil;
        --lv.count;
        o.live = false;
        --live_;
        release(s);
      }
    }
    return remaining;
  }

  Price min_price_;
  Price max_price_;
  std::size_t n_levels_;
  std::vector<Level> bid_levels_;
  std::vector<Level> ask_levels_;
  LevelBitmap bid_bits_;
  LevelBitmap ask_bits_;
  std::size_t best_bid_ = LevelBitmap::npos;
  std::size_t best_ask_ = LevelBitmap::npos;
  std::vector<Order> orders_;
  std::vector<std::uint32_t> free_;
  std::vector<Fill> fills_;
  std::size_t live_ = 0;
};

}  // namespace tf
