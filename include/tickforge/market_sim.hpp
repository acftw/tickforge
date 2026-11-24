// Agent-based market simulator built on the matching engine.
//
// The market has a hidden "fundamental" value V that jumps by one tick at
// random times. Four kinds of participants trade through one OrderBook:
//
//   * Liquidity providers post limit orders near the touch and cancel them at
//     random. They know nothing about V.
//   * Noise traders send market orders in random directions.
//   * Informed traders know V. When the best ask is below V they buy it (and
//     symmetrically sell a bid above V), so they pick off stale quotes. This
//     is the adverse selection a market maker has to survive.
//   * One market maker (the strategy under test) re-quotes on a timer. Its
//     orders reach the book after a configurable latency, and it joins the
//     back of the queue like everyone else. It can optionally watch a "lead"
//     signal that moves with V, like a futures contract that reprices before
//     the stock does. Using that signal well is a race: the maker has to pull
//     its stale quote before an informed trader picks it off.
//
// Time is continuous. Background events follow Poisson processes and are
// simulated exactly (Gillespie's method); the market maker's timer and its
// delayed orders are scheduled events.
#pragma once

#include <cstdint>
#include <vector>

#include "tickforge/order_book.hpp"

namespace tf {

enum class Strategy : std::uint8_t { None = 0, Symmetric = 1, Glft = 2 };

enum OwnerType : Owner { kMaker = 1, kLiquidity = 2, kNoise = 3, kInformed = 4 };

struct MarketConfig {
  double duration = 3600.0;      // recorded trading time, seconds
  double warmup = 120.0;         // book build-up before recording and quoting start
  Price start_price = 10000;     // ticks
  Price price_band = 20000;      // book covers start_price +/- band ticks
  double lp_rate = 5.0;          // liquidity-provider limit orders per second, per side
  double lp_cancel_rate = 0.1;   // cancel rate per resting liquidity order, per second
  double lp_offset_p = 0.45;     // offset from the opposite touch: 1 + Geometric(p) ticks
  int lp_max_size = 5;           // order sizes uniform on 1..max
  double noise_rate = 2.0;       // noise market orders per second (both sides)
  int noise_max_size = 4;
  double informed_rate = 5.0;    // informed trader arrivals per second (mean reaction 200 ms)
  int informed_max_size = 50;    // informed traders sweep every mispriced order up to this size
  double jump_rate = 0.1;        // fundamental jumps (+/-1 tick) per second
  double sample_interval = 0.1;  // recording grid, seconds
  int depth_window = 0;          // if > 0, record resting depth at start_price +/- this many ticks
};

struct MakerConfig {
  Strategy strategy = Strategy::None;
  double update_interval = 0.01;  // seconds between quoting decisions
  double latency = 0.0;           // seconds from decision to order reaching the book
  Qty quote_size = 2;
  Qty max_inventory = 20;
  double half_spread = 1.0;       // Symmetric: quote mid +/- half_spread ticks
  double gamma = 0.05;            // GLFT risk aversion
  double kappa = 1.0;             // GLFT fill-intensity decay, per tick
  double A = 1.0;                 // GLFT fill intensity at zero distance, per second
  double sigma = 0.3;             // mid-price volatility, ticks per sqrt(second)
  // Fair-value adjustments (GLFT only). Fair value used for quoting is
  //   mid + imbalance_coef * queue_imbalance + lead_coef * (lead_signal - mid)
  double imbalance_coef = 0.0;
  double lead_coef = 0.0;
};

struct Quotes {
  bool has_bid = false;
  bool has_ask = false;
  Price bid = 0;
  Price ask = 0;
};

// Pure quoting rule, exposed separately so it can be unit-tested.
struct BookState {
  bool has_bid, has_ask;
  Price best_bid, best_ask;
  Qty bid_qty, ask_qty;
  double mid;
  double lead = 0.0;  // lead signal (the fundamental value as seen in a leading market)
};
Quotes compute_quotes(const MakerConfig& cfg, const BookState& s, Qty inventory);

struct SimResult {
  // Recording grid (time measured from the end of warm-up).
  std::vector<double> t, mid, fundamental, spread, inventory, pnl;
  // Maker's live quote prices on the grid (NaN when it has no order on that side).
  std::vector<double> maker_bid, maker_ask;
  // Resting depth on the grid (only if depth_window > 0): for each sample, 2*depth_window+1
  // values for prices start_price-depth_window ... start_price+depth_window.
  // Positive = bid quantity, negative = ask quantity (a price can't hold both).
  std::vector<std::int32_t> depth;
  // Market-maker fills. side = +1 when the maker bought.
  std::vector<double> fill_t, fill_price, fill_mid, fill_fundamental;
  std::vector<int> fill_side, fill_taker;
  std::vector<std::int64_t> fill_qty;
  // Every trade on the tape. side = +1 for a buyer-initiated trade.
  std::vector<double> trade_t, trade_price, trade_mid_before, trade_fundamental;
  std::vector<int> trade_side, trade_taker, trade_maker;
  std::vector<std::int64_t> trade_qty;
  // Totals.
  std::int64_t events = 0, maker_orders = 0, maker_cancels = 0;
  double final_cash = 0.0, final_mid = 0.0, final_fundamental = 0.0;
  std::int64_t final_inventory = 0;
};

SimResult simulate(const MarketConfig& market, const MakerConfig& maker, std::uint64_t seed);

}  // namespace tf
