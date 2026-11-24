#include <random>
#include <vector>

#include "doctest/doctest.h"
#include "tickforge/level_bitmap.hpp"
#include "tickforge/order_book.hpp"
#include "tickforge/reference_book.hpp"

using namespace tf;

TEST_CASE("bitmap next/prev agree with a linear scan") {
  std::mt19937_64 rng(7);
  for (std::size_t n : {1u, 63u, 64u, 65u, 4095u, 4096u, 4097u, 70000u}) {
    LevelBitmap bm(n);
    std::vector<bool> ref(n, false);
    for (int step = 0; step < 4000; ++step) {
      const std::size_t i = rng() % n;
      if (rng() % 2) { bm.set(i); ref[i] = true; } else { bm.clear(i); ref[i] = false; }
      const std::size_t q = rng() % n;
      std::size_t want_next = LevelBitmap::npos, want_prev = LevelBitmap::npos;
      for (std::size_t j = q; j < n; ++j) if (ref[j]) { want_next = j; break; }
      for (std::size_t j = q + 1; j-- > 0;) if (ref[j]) { want_prev = j; break; }
      REQUIRE(bm.next(q) == want_next);
      REQUIRE(bm.prev(q) == want_prev);
    }
  }
}

TEST_CASE("price-time priority: best price first, then oldest first") {
  OrderBook b(1, 1000);
  auto a1 = b.add_limit(Side::Sell, 101, 5, 1);
  auto a2 = b.add_limit(Side::Sell, 100, 3, 2);
  auto a3 = b.add_limit(Side::Sell, 100, 4, 3);
  CHECK(b.best_ask() == 100);
  CHECK(b.depth(Side::Sell, 100) == 7);

  auto t = b.add_limit(Side::Buy, 101, 9, 9);
  REQUIRE(b.fills().size() == 3);
  CHECK(b.fills()[0].maker_id == a2.id);  // 100, oldest
  CHECK(b.fills()[0].qty == 3);
  CHECK(b.fills()[1].maker_id == a3.id);  // 100, next
  CHECK(b.fills()[1].qty == 4);
  CHECK(b.fills()[2].maker_id == a1.id);  // 101
  CHECK(b.fills()[2].qty == 2);
  CHECK(b.fills()[2].price == 101);       // trades at the maker's price
  CHECK(t.filled == 9);
  CHECK(t.resting == 0);
  CHECK(b.order_qty(a1.id) == 3);
  CHECK_FALSE(b.is_live(a2.id));
}

TEST_CASE("unfilled GTC remainder rests; IOC remainder is dropped") {
  OrderBook b(1, 1000);
  b.add_limit(Side::Sell, 100, 2);
  auto gtc = b.add_limit(Side::Buy, 100, 5);
  CHECK(gtc.filled == 2);
  CHECK(gtc.resting == 3);
  CHECK(b.best_bid() == 100);
  CHECK_FALSE(b.has_ask());

  b.add_limit(Side::Sell, 105, 1);
  auto ioc = b.add_limit(Side::Buy, 106, 4, 0, Tif::IOC);
  CHECK(ioc.filled == 1);
  CHECK(ioc.resting == 0);
  CHECK_FALSE(b.is_live(ioc.id));
  CHECK(b.best_bid() == 100);
}

TEST_CASE("market order sweeps levels and never rests") {
  OrderBook b(1, 1000);
  b.add_limit(Side::Buy, 99, 2);
  b.add_limit(Side::Buy, 98, 2);
  auto m = b.add_market(Side::Sell, 10);
  CHECK(m.filled == 4);
  CHECK(m.resting == 0);
  CHECK_FALSE(b.has_bid());
  CHECK(b.live_orders() == 0);
}

TEST_CASE("cancel, stale ids and reduce keeping queue position") {
  OrderBook b(1, 1000);
  auto o1 = b.add_limit(Side::Buy, 50, 10);
  auto o2 = b.add_limit(Side::Buy, 50, 10);
  CHECK(b.queue_ahead(o2.id) == 10);
  CHECK(b.reduce(o1.id, 4));
  CHECK(b.queue_ahead(o2.id) == 4);
  CHECK(b.depth(Side::Buy, 50) == 14);
  CHECK_FALSE(b.reduce(o1.id, 6));  // cannot increase

  b.add_market(Side::Sell, 5);
  CHECK(b.fills()[0].maker_id == o1.id);  // o1 kept priority after reduce
  CHECK_FALSE(b.is_live(o1.id));
  CHECK(b.order_qty(o2.id) == 9);

  CHECK(b.cancel(o2.id));
  CHECK_FALSE(b.cancel(o2.id));        // already gone
  auto o3 = b.add_limit(Side::Buy, 50, 1);  // likely reuses a freed slot
  CHECK_FALSE(b.cancel(o1.id));        // stale id must not cancel o3
  CHECK(b.is_live(o3.id));
}

TEST_CASE("best price recovers after the best level empties") {
  OrderBook b(1, 100000);
  auto far = b.add_limit(Side::Sell, 90000, 1);
  auto near = b.add_limit(Side::Sell, 10, 1);
  CHECK(b.best_ask() == 10);
  b.cancel(near.id);
  CHECK(b.best_ask() == 90000);
  b.cancel(far.id);
  CHECK_FALSE(b.has_ask());
}

TEST_CASE("rejects bad input") {
  OrderBook b(100, 200);
  CHECK(b.add_limit(Side::Buy, 99, 1).rejected);
  CHECK(b.add_limit(Side::Buy, 201, 1).rejected);
  CHECK(b.add_limit(Side::Buy, 150, 0).rejected);
  CHECK(b.live_orders() == 0);
}

// The fast book and the simple reference book must agree on every fill and on
// the state of the book after every one of hundreds of thousands of random
// operations.
TEST_CASE("differential test against the reference book") {
  for (std::uint64_t seed : {1ull, 2ull, 3ull, 4ull}) {
    std::mt19937_64 rng(seed);
    OrderBook fast(1, 2000);
    ReferenceBook ref;
    std::vector<OrderId> fast_id;          // by submission index
    std::unordered_map<OrderId, std::uint64_t> fast_to_ref;
    long fills_checked = 0;

    for (int step = 0; step < 200000; ++step) {
      const int kind = static_cast<int>(rng() % 100);
      if (kind < 55 || fast_id.empty()) {
        const Side side = rng() % 2 ? Side::Buy : Side::Sell;
        const Price mid = 1000;
        const Price px = side == Side::Buy ? mid - static_cast<Price>(rng() % 12) + 3
                                           : mid + static_cast<Price>(rng() % 12) - 3;
        const Qty qty = 1 + static_cast<Qty>(rng() % 9);
        const bool ioc = rng() % 10 == 0;
        auto r = fast.add_limit(side, px, qty, 0, ioc ? Tif::IOC : Tif::GTC);
        const std::uint64_t rid = ref.add_limit(side, px, qty, ioc);
        REQUIRE(fast.fills().size() == ref.fills.size());
        for (std::size_t k = 0; k < ref.fills.size(); ++k) {
          const Fill& f = fast.fills()[k];
          REQUIRE(fast_to_ref.at(f.maker_id) == ref.fills[k].maker_id);
          REQUIRE(f.price == ref.fills[k].price);
          REQUIRE(f.qty == ref.fills[k].qty);
          ++fills_checked;
        }
        if (r.resting > 0) {
          fast_to_ref[r.id] = rid;
          fast_id.push_back(r.id);
        }
      } else {
        const std::size_t k = rng() % fast_id.size();
        const OrderId fid = fast_id[k];
        const std::uint64_t rid = fast_to_ref.at(fid);
        if (kind < 90) {
          REQUIRE(fast.cancel(fid) == ref.cancel(rid));
        } else {
          const Qty nq = static_cast<Qty>(rng() % 5);
          REQUIRE(fast.reduce(fid, nq) == ref.reduce(rid, nq));
        }
        if (!fast.is_live(fid)) {
          fast_id[k] = fast_id.back();
          fast_id.pop_back();
        }
      }
      // Compare full book state.
      REQUIRE(fast.has_bid() == ref.has_bid());
      REQUIRE(fast.has_ask() == ref.has_ask());
      if (fast.has_bid()) REQUIRE(fast.best_bid() == ref.best_bid());
      if (fast.has_ask()) REQUIRE(fast.best_ask() == ref.best_ask());
      if (fast.has_bid() && fast.has_ask()) REQUIRE(fast.best_bid() < fast.best_ask());
      REQUIRE(fast.live_orders() == ref.live_orders());
      if (step % 97 == 0) {
        for (Price p = 980; p <= 1020; ++p) {
          REQUIRE(fast.depth(Side::Buy, p) == ref.depth(Side::Buy, p));
          REQUIRE(fast.depth(Side::Sell, p) == ref.depth(Side::Sell, p));
        }
      }
      // Drop fills' makers that are no longer live from the id list lazily.
      if (fast_id.size() > 4096) {
        std::vector<OrderId> keep;
        for (OrderId id : fast_id) if (fast.is_live(id)) keep.push_back(id);
        fast_id.swap(keep);
      }
    }
    CHECK(fills_checked > 10000);
    MESSAGE("seed " << seed << ": " << fills_checked << " fills matched the reference book");
  }
}
