// A deliberately simple order book used only to check the fast one.
//
// It uses the textbook data structures (std::map of price -> std::deque of
// orders, plus a hash map from id to order) and the most direct matching loop
// possible. It shares no code with OrderBook, so if both produce the same
// fills for millions of random orders, the fast book is very likely correct.
// It is also the baseline in the benchmark.
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include "tickforge/order_book.hpp"

namespace tf {

class ReferenceBook {
 public:
  struct RefFill {
    std::uint64_t maker_id;
    std::uint64_t taker_id;
    Price price;
    Qty qty;
  };

  // Returns this order's id (ids are 1, 2, 3, ... in submission order).
  std::uint64_t add_limit(Side side, Price price, Qty qty, bool ioc = false) {
    fills.clear();
    const std::uint64_t id = next_id_++;
    Qty remaining = qty;
    if (side == Side::Buy) {
      while (remaining > 0 && !asks_.empty() && asks_.begin()->first <= price) {
        remaining = match(asks_.begin()->second, asks_.begin()->first, remaining, id);
        if (asks_.begin()->second.empty()) asks_.erase(asks_.begin());
      }
    } else {
      while (remaining > 0 && !bids_.empty() && bids_.begin()->first >= price) {
        remaining = match(bids_.begin()->second, bids_.begin()->first, remaining, id);
        if (bids_.begin()->second.empty()) bids_.erase(bids_.begin());
      }
    }
    if (remaining > 0 && !ioc) {
      if (side == Side::Buy) bids_[price].push_back({id, remaining});
      else asks_[price].push_back({id, remaining});
      where_[id] = {side, price};
    }
    return id;
  }

  bool cancel(std::uint64_t id) {
    auto it = where_.find(id);
    if (it == where_.end()) return false;
    const auto [side, price] = it->second;
    if (side == Side::Buy) erase_from(bids_, price, id); else erase_from(asks_, price, id);
    where_.erase(it);
    return true;
  }

  bool reduce(std::uint64_t id, Qty new_qty) {
    auto it = where_.find(id);
    if (it == where_.end()) return false;
    if (new_qty <= 0) return cancel(id);
    const auto [side, price] = it->second;
    auto& q = side == Side::Buy ? bids_[price] : asks_[price];
    for (auto& o : q) {
      if (o.id == id) {
        if (new_qty >= o.qty) return false;
        o.qty = new_qty;
        return true;
      }
    }
    return false;
  }

  bool has_bid() const { return !bids_.empty(); }
  bool has_ask() const { return !asks_.empty(); }
  Price best_bid() const { return bids_.begin()->first; }
  Price best_ask() const { return asks_.begin()->first; }
  std::size_t live_orders() const { return where_.size(); }

  Qty depth(Side side, Price price) const {
    Qty total = 0;
    if (side == Side::Buy) {
      auto it = bids_.find(price);
      if (it != bids_.end()) for (const auto& o : it->second) total += o.qty;
    } else {
      auto it = asks_.find(price);
      if (it != asks_.end()) for (const auto& o : it->second) total += o.qty;
    }
    return total;
  }

  std::vector<RefFill> fills;

 private:
  struct RefOrder {
    std::uint64_t id;
    Qty qty;
  };

  Qty match(std::deque<RefOrder>& q, Price price, Qty remaining, std::uint64_t taker) {
    while (remaining > 0 && !q.empty()) {
      RefOrder& o = q.front();
      const Qty take = std::min(remaining, o.qty);
      fills.push_back({o.id, taker, price, take});
      o.qty -= take;
      remaining -= take;
      if (o.qty == 0) {
        where_.erase(o.id);
        q.pop_front();
      }
    }
    return remaining;
  }

  template <class Map>
  void erase_from(Map& m, Price price, std::uint64_t id) {
    auto lv = m.find(price);
    auto& q = lv->second;
    q.erase(std::find_if(q.begin(), q.end(), [&](const RefOrder& o) { return o.id == id; }));
    if (q.empty()) m.erase(lv);
  }

  std::map<Price, std::deque<RefOrder>, std::greater<Price>> bids_;
  std::map<Price, std::deque<RefOrder>> asks_;
  std::unordered_map<std::uint64_t, std::pair<Side, Price>> where_;
  std::uint64_t next_id_ = 1;
};

}  // namespace tf
