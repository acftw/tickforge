#include <cmath>
#include <numeric>

#include "doctest/doctest.h"
#include "tickforge/market_sim.hpp"

using namespace tf;

namespace {
MakerConfig glft() {
  MakerConfig k;
  k.strategy = Strategy::Glft;
  k.gamma = 0.1;
  k.kappa = 1.0;
  k.A = 2.0;
  k.sigma = 1.0;
  k.quote_size = 1;
  k.max_inventory = 10;
  return k;
}
BookState wide_book() { return BookState{true, true, 90, 110, 5, 5, 100.0, 100.0}; }
}  // namespace

TEST_CASE("GLFT quotes: symmetric when flat, skewed against inventory") {
  const MakerConfig k = glft();
  const Quotes flat = compute_quotes(k, wide_book(), 0);
  CHECK(flat.has_bid);
  CHECK(flat.has_ask);
  CHECK(100 - flat.bid == flat.ask - 100);

  const Quotes longq = compute_quotes(k, wide_book(), 6);
  CHECK(longq.bid < flat.bid);   // long: buy less eagerly
  CHECK(longq.ask <= flat.ask);  // and sell more eagerly
  const Quotes shortq = compute_quotes(k, wide_book(), -6);
  CHECK(shortq.ask > flat.ask);
  CHECK(shortq.bid >= flat.bid);
}

TEST_CASE("quotes respect inventory limits and never cross the book") {
  MakerConfig k = glft();
  CHECK_FALSE(compute_quotes(k, wide_book(), 10).has_bid);
  CHECK_FALSE(compute_quotes(k, wide_book(), -10).has_ask);
  const BookState tight{true, true, 100, 101, 3, 3, 100.5, 100.5};
  for (int q = -9; q <= 9; ++q) {
    const Quotes x = compute_quotes(k, tight, q);
    CHECK(x.bid <= 100);
    CHECK(x.ask >= 101);
    CHECK(x.bid < x.ask);
  }
}

TEST_CASE("queue imbalance shifts the fair value toward the heavier side") {
  MakerConfig k = glft();
  k.imbalance_coef = 3.0;
  const Quotes bid_heavy = compute_quotes(k, BookState{true, true, 90, 110, 9, 1, 100.0, 100.0}, 0);
  const Quotes ask_heavy = compute_quotes(k, BookState{true, true, 90, 110, 1, 9, 100.0, 100.0}, 0);
  CHECK(bid_heavy.bid > ask_heavy.bid);
  CHECK(bid_heavy.ask > ask_heavy.ask);
}

TEST_CASE("lead signal moves quotes with the leading market") {
  MakerConfig k = glft();
  k.lead_coef = 1.0;
  const Quotes up = compute_quotes(k, BookState{true, true, 90, 110, 5, 5, 100.0, 104.0}, 0);
  const Quotes same = compute_quotes(k, BookState{true, true, 90, 110, 5, 5, 100.0, 100.0}, 0);
  CHECK(up.bid == same.bid + 4);
  CHECK(up.ask == same.ask + 4);
}

TEST_CASE("simulation accounting, limits and determinism") {
  MarketConfig m;
  m.duration = 900;
  MakerConfig k = glft();
  k.latency = 0.02;
  const SimResult a = simulate(m, k, 42);
  const SimResult b = simulate(m, k, 42);
  CHECK(a.fill_t.size() == b.fill_t.size());
  CHECK(a.final_cash == b.final_cash);

  // Inventory and cash follow exactly from the recorded fills.
  std::int64_t inv = 0;
  double cash = 0;
  for (std::size_t i = 0; i < a.fill_t.size(); ++i) {
    inv += a.fill_side[i] * a.fill_qty[i];
    cash -= a.fill_side[i] * a.fill_price[i] * static_cast<double>(a.fill_qty[i]);
  }
  CHECK(inv == a.final_inventory);
  CHECK(cash == doctest::Approx(a.final_cash));
  CHECK(a.fill_t.size() > 20);

  for (double q : a.inventory) CHECK(std::abs(q) <= k.max_inventory);

  // Informed traders keep the visible price close to the hidden value.
  double err = 0;
  for (std::size_t i = 0; i < a.mid.size(); ++i) err += std::abs(a.mid[i] - a.fundamental[i]);
  CHECK(err / static_cast<double>(a.mid.size()) < 1.5);

  // The book is almost always two-sided.
  std::size_t nan = 0;
  for (double s : a.spread) nan += std::isnan(s) ? 1 : 0;
  CHECK(nan < a.spread.size() / 100);
}

TEST_CASE("no market maker means no maker fills") {
  MarketConfig m;
  m.duration = 300;
  const SimResult r = simulate(m, MakerConfig{}, 1);
  CHECK(r.fill_t.empty());
  CHECK(r.trade_t.size() > 100);
  CHECK(r.final_inventory == 0);
}

TEST_CASE("depth recording: bids below asks at every sample") {
  MarketConfig m;
  m.duration = 120;
  m.sample_interval = 1.0;
  m.depth_window = 30;
  const SimResult r = simulate(m, MakerConfig{}, 9);
  const std::size_t w = 2 * 30 + 1;
  REQUIRE(r.depth.size() == r.t.size() * w);
  for (std::size_t i = 0; i < r.t.size(); ++i) {
    // Inside the window every bid level must sit below every ask level.
    int highest_bid = -1, lowest_ask = static_cast<int>(w);
    for (std::size_t j = 0; j < w; ++j) {
      const int d = r.depth[i * w + j];
      if (d > 0) highest_bid = static_cast<int>(j);
      if (d < 0 && lowest_ask == static_cast<int>(w)) lowest_ask = static_cast<int>(j);
    }
    CHECK(highest_bid < lowest_ask);
  }
  MarketConfig off = m;
  off.depth_window = 0;
  CHECK(simulate(off, MakerConfig{}, 9).depth.empty());
}
