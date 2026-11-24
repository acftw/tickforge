#include "tickforge/market_sim.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_map>

namespace tf {
namespace {

// Platform-independent random draws (std:: distributions differ between
// standard libraries, which would make results depend on the compiler).
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : eng_(seed) {}
  double uniform() { return static_cast<double>(eng_() >> 11) * 0x1.0p-53; }  // [0, 1)
  double exponential(double rate) { return -std::log1p(-uniform()) / rate; }
  int uniform_int(int lo, int hi) { return lo + static_cast<int>(uniform() * (hi - lo + 1)); }
  int geometric(double p) {  // failures before first success, support 0, 1, 2, ...
    const double u = 1.0 - uniform();  // (0, 1]
    return static_cast<int>(std::floor(std::log(u) / std::log1p(-p)));
  }
  bool coin() { return (eng_() >> 63) != 0; }

 private:
  std::mt19937_64 eng_;
};

struct PendingQuote {
  double time;
  Quotes quotes;
};

class Simulator {
 public:
  Simulator(const MarketConfig& m, const MakerConfig& k, std::uint64_t seed)
      : m_(m),
        k_(k),
        rng_(seed),
        book_(m.start_price - m.price_band, m.start_price + m.price_band, 1 << 14),
        fundamental_(m.start_price),
        last_mid_(m.start_price) {
    if (m.duration <= 0 || m.sample_interval <= 0) throw std::invalid_argument("bad market config");
    if (k.strategy != Strategy::None && (k.update_interval <= 0 || k.latency < 0 || k.quote_size <= 0))
      throw std::invalid_argument("bad maker config");
  }

  SimResult run() {
    const double t_end = m_.warmup + m_.duration;
    double next_sample = m_.warmup;
    double next_decision = k_.strategy == Strategy::None ? kInf : m_.warmup;
    double t = 0.0;

    for (;;) {
      const double rate = 2.0 * m_.lp_rate + m_.noise_rate + m_.informed_rate + m_.jump_rate +
                          m_.lp_cancel_rate * static_cast<double>(lp_orders_.size());
      const double t_exo = t + rng_.exponential(rate);
      const double t_pending = pending_.empty() ? kInf : pending_.front().time;
      const double t_sched = std::min({next_sample, next_decision, t_pending});

      if (t_sched <= t_exo && t_sched <= t_end) {
        // A scheduled event comes first. The exponential draw is discarded,
        // which is exact because Poisson arrivals are memoryless.
        t = t_sched;
        if (t_sched == next_sample) {
          record_sample(t);
          next_sample += m_.sample_interval;
        } else if (t_sched == t_pending) {
          const Quotes q = pending_.front().quotes;
          pending_.pop_front();
          apply_quotes(q);
        } else {
          decide(t);
          next_decision += k_.update_interval;
        }
        continue;
      }
      if (t_exo > t_end) break;
      t = t_exo;
      ++res_.events;
      exogenous_event(t, rate);
    }

    res_.final_cash = cash_;
    res_.final_inventory = inventory_;
    res_.final_mid = current_mid();
    res_.final_fundamental = static_cast<double>(fundamental_);
    return std::move(res_);
  }

 private:
  static constexpr double kInf = std::numeric_limits<double>::infinity();

  double current_mid() {
    if (book_.has_bid() && book_.has_ask()) last_mid_ = 0.5 * (book_.best_bid() + book_.best_ask());
    return last_mid_;
  }

  Price clamp(Price p) const { return std::clamp(p, book_.min_price(), book_.max_price()); }

  void exogenous_event(double t, double rate) {
    double u = rng_.uniform() * rate;
    if ((u -= m_.lp_rate) < 0) return lp_order(t, Side::Buy);
    if ((u -= m_.lp_rate) < 0) return lp_order(t, Side::Sell);
    if ((u -= m_.noise_rate) < 0) {
      const Side s = rng_.coin() ? Side::Buy : Side::Sell;
      const double mid = current_mid();
      book_.add_market(s, rng_.uniform_int(1, m_.noise_max_size), kNoise);
      return on_fills(t, mid);
    }
    if ((u -= m_.informed_rate) < 0) return informed(t);
    if ((u -= m_.jump_rate) < 0) {
      fundamental_ += rng_.coin() ? 1 : -1;
      return;
    }
    lp_cancel();
  }

  void lp_order(double t, Side side) {
    const int offset = 1 + rng_.geometric(m_.lp_offset_p);
    Price px;
    if (side == Side::Buy) {
      const Price ref = book_.has_ask() ? book_.best_ask()
                        : book_.has_bid() ? book_.best_bid() + 1
                                          : static_cast<Price>(fundamental_ + 1);
      px = clamp(ref - offset);
    } else {
      const Price ref = book_.has_bid() ? book_.best_bid()
                        : book_.has_ask() ? book_.best_ask() - 1
                                          : static_cast<Price>(fundamental_ - 1);
      px = clamp(ref + offset);
    }
    const double mid = current_mid();
    const AddResult r = book_.add_limit(side, px, rng_.uniform_int(1, m_.lp_max_size), kLiquidity);
    on_fills(t, mid);  // only possible if a band edge was hit; kept for safety
    if (r.resting > 0) {
      lp_pos_[r.id] = lp_orders_.size();
      lp_orders_.push_back(r.id);
    }
  }

  void lp_cancel() {
    if (lp_orders_.empty()) return;
    const std::size_t i = static_cast<std::size_t>(rng_.uniform() * lp_orders_.size());
    const OrderId id = lp_orders_[i];
    book_.cancel(id);
    remove_lp(id);
  }

  void remove_lp(OrderId id) {
    auto it = lp_pos_.find(id);
    if (it == lp_pos_.end()) return;
    const std::size_t i = it->second;
    const OrderId last = lp_orders_.back();
    lp_orders_[i] = last;
    lp_pos_[last] = i;
    lp_orders_.pop_back();
    lp_pos_.erase(id);
  }

  void informed(double t) {
    const Qty size = rng_.uniform_int(1, m_.informed_max_size);
    const double mid = current_mid();
    if (book_.has_ask() && book_.best_ask() < fundamental_) {
      book_.add_limit(Side::Buy, clamp(static_cast<Price>(fundamental_ - 1)), size, kInformed, Tif::IOC);
      on_fills(t, mid);
    } else if (book_.has_bid() && book_.best_bid() > fundamental_) {
      book_.add_limit(Side::Sell, clamp(static_cast<Price>(fundamental_ + 1)), size, kInformed, Tif::IOC);
      on_fills(t, mid);
    }
  }

  void on_fills(double t, double mid_before) {
    const bool recording = t >= m_.warmup;
    for (const Fill& f : book_.fills()) {
      const int sign = f.taker_side == Side::Buy ? 1 : -1;
      if (recording) {
        res_.trade_t.push_back(t - m_.warmup);
        res_.trade_price.push_back(f.price);
        res_.trade_mid_before.push_back(mid_before);
        res_.trade_side.push_back(sign);
        res_.trade_taker.push_back(static_cast<int>(f.taker_owner));
        res_.trade_maker.push_back(static_cast<int>(f.maker_owner));
        res_.trade_fundamental.push_back(static_cast<double>(fundamental_));
        res_.trade_qty.push_back(f.qty);
      }
      if (f.maker_owner == kMaker) {
        // The maker is on the other side of the taker.
        inventory_ -= sign * f.qty;
        cash_ += sign * static_cast<double>(f.price) * static_cast<double>(f.qty);
        res_.fill_t.push_back(t - m_.warmup);
        res_.fill_price.push_back(f.price);
        res_.fill_mid.push_back(mid_before);
        res_.fill_fundamental.push_back(static_cast<double>(fundamental_));
        res_.fill_side.push_back(-sign);
        res_.fill_taker.push_back(static_cast<int>(f.taker_owner));
        res_.fill_qty.push_back(f.qty);
      } else if (f.maker_owner == kLiquidity && !book_.is_live(f.maker_id)) {
        remove_lp(f.maker_id);
      }
    }
    current_mid();
  }

  void record_sample(double t) {
    const double mid = current_mid();
    res_.t.push_back(t - m_.warmup);
    res_.mid.push_back(mid);
    res_.fundamental.push_back(static_cast<double>(fundamental_));
    res_.spread.push_back(book_.has_bid() && book_.has_ask()
                              ? static_cast<double>(book_.best_ask() - book_.best_bid())
                              : std::numeric_limits<double>::quiet_NaN());
    res_.inventory.push_back(static_cast<double>(inventory_));
    res_.pnl.push_back(cash_ + static_cast<double>(inventory_) * mid);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    res_.maker_bid.push_back(bid_id_ != kNoOrder && book_.is_live(bid_id_) ? book_.order_price(bid_id_) : nan);
    res_.maker_ask.push_back(ask_id_ != kNoOrder && book_.is_live(ask_id_) ? book_.order_price(ask_id_) : nan);
  }

  void decide(double t) {
    const BookState s{book_.has_bid(),
                      book_.has_ask(),
                      book_.has_bid() ? book_.best_bid() : 0,
                      book_.has_ask() ? book_.best_ask() : 0,
                      book_.best_bid_qty(),
                      book_.best_ask_qty(),
                      current_mid(),
                      static_cast<double>(fundamental_)};
    const Quotes q = compute_quotes(k_, s, inventory_);
    if (k_.latency <= 0.0) {
      apply_quotes(q);
    } else {
      pending_.push_back({t + k_.latency, q});
    }
  }

  // Move the maker's live orders to the target quotes. An order already
  // resting at the target price is left alone so it keeps its queue position.
  void apply_quotes(const Quotes& q) {
    // Risk check at the gateway with the inventory as it is now.
    const bool allow_bid = q.has_bid && inventory_ + k_.quote_size <= k_.max_inventory;
    const bool allow_ask = q.has_ask && inventory_ - k_.quote_size >= -k_.max_inventory;
    update_side(Side::Buy, allow_bid, q.bid, bid_id_);
    update_side(Side::Sell, allow_ask, q.ask, ask_id_);
  }

  void update_side(Side side, bool want, Price px, OrderId& id) {
    const bool live = id != kNoOrder && book_.is_live(id);
    if (live && want && book_.order_price(id) == px) return;
    if (live) {
      book_.cancel(id);
      ++res_.maker_cancels;
    }
    id = kNoOrder;
    if (!want) return;
    // Post-only: never cross the spread (the maker only provides liquidity).
    if (side == Side::Buy && book_.has_ask() && px >= book_.best_ask()) return;
    if (side == Side::Sell && book_.has_bid() && px <= book_.best_bid()) return;
    px = clamp(px);
    const AddResult r = book_.add_limit(side, px, k_.quote_size, kMaker);
    ++res_.maker_orders;
    id = r.resting > 0 ? r.id : kNoOrder;
  }

  MarketConfig m_;
  MakerConfig k_;
  Rng rng_;
  OrderBook book_;
  std::int64_t fundamental_;
  double last_mid_;
  std::vector<OrderId> lp_orders_;
  std::unordered_map<OrderId, std::size_t> lp_pos_;
  std::deque<PendingQuote> pending_;
  OrderId bid_id_ = kNoOrder;
  OrderId ask_id_ = kNoOrder;
  std::int64_t inventory_ = 0;
  double cash_ = 0.0;
  SimResult res_;
};

}  // namespace

Quotes compute_quotes(const MakerConfig& c, const BookState& s, Qty inventory) {
  Quotes out;
  if (c.strategy == Strategy::None) return out;

  double fair = s.mid;
  double delta_bid = c.half_spread;
  double delta_ask = c.half_spread;

  if (c.strategy == Strategy::Glft) {
    // Gueant, Lehalle & Fernandez-Tapia (2013) closed-form approximation of
    // the optimal quotes for an inventory-averse market maker:
    //   delta_bid = c1 + (2q + 1)/2 * omega,   delta_ask = c1 - (2q - 1)/2 * omega
    //   c1 = ln(1 + gamma/kappa) / gamma
    //   omega = sqrt( sigma^2 gamma / (2 kappa A) * (1 + gamma/kappa)^(1 + kappa/gamma) )
    // with q measured in lots of quote_size.
    const double g = c.gamma, k = c.kappa;
    const double c1 = std::log1p(g / k) / g;
    const double omega =
        std::sqrt(c.sigma * c.sigma * g / (2.0 * k * c.A) * std::pow(1.0 + g / k, 1.0 + k / g));
    const double q = static_cast<double>(inventory) / static_cast<double>(c.quote_size);
    delta_bid = c1 + (2.0 * q + 1.0) / 2.0 * omega;
    delta_ask = c1 - (2.0 * q - 1.0) / 2.0 * omega;
    if (c.imbalance_coef != 0.0 && s.bid_qty + s.ask_qty > 0) {
      const double imb = static_cast<double>(s.bid_qty - s.ask_qty) / static_cast<double>(s.bid_qty + s.ask_qty);
      fair += c.imbalance_coef * imb;
    }
    fair += c.lead_coef * (s.lead - s.mid);
  }

  // Round to ticks; halves round toward the fair value (more aggressive).
  Price bid = static_cast<Price>(std::floor(fair - delta_bid + 0.5));
  Price ask = static_cast<Price>(std::ceil(fair + delta_ask - 0.5));
  if (s.has_ask) bid = std::min(bid, static_cast<Price>(s.best_ask - 1));
  if (s.has_bid) ask = std::max(ask, static_cast<Price>(s.best_bid + 1));
  if (bid >= ask) {  // keep the maker's own quotes uncrossed
    if (inventory > 0) bid = ask - 1; else ask = bid + 1;
  }
  out.has_bid = inventory + c.quote_size <= c.max_inventory;
  out.has_ask = inventory - c.quote_size >= -c.max_inventory;
  out.bid = bid;
  out.ask = ask;
  return out;
}

SimResult simulate(const MarketConfig& market, const MakerConfig& maker, std::uint64_t seed) {
  return Simulator(market, maker, seed).run();
}

}  // namespace tf
