// Benchmark: replay the same realistic message stream through the fast book
// and through the std::map reference book, and report throughput plus
// per-message latency percentiles.
//
// The stream mimics a busy instrument: ~55% new limit orders clustered near
// the touch, ~38% cancels of random live orders, ~7% marketable orders, with a
// slowly drifting mid price.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "tickforge/order_book.hpp"
#include "tickforge/reference_book.hpp"

using namespace tf;
using Clock = std::chrono::steady_clock;

namespace {

struct Msg {
  enum Kind : std::uint8_t { Add, Cancel, Market } kind;
  Side side;
  Price price;
  Qty qty;
  std::uint32_t target;  // for cancels: index of the add message to cancel
};

std::vector<Msg> make_stream(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<Msg> out;
  out.reserve(n);
  std::vector<std::uint32_t> adds;
  double mid = 50000;
  for (std::size_t i = 0; i < n; ++i) {
    mid += (rng() % 1000 == 0) ? (rng() % 2 ? 1.0 : -1.0) : 0.0;
    const unsigned r = rng() % 100;
    if (r < 55 || adds.empty()) {
      const Side s = rng() % 2 ? Side::Buy : Side::Sell;
      const int off = static_cast<int>(rng() % 3 + (rng() % 4 == 0 ? rng() % 40 : 0));
      const Price p = s == Side::Buy ? static_cast<Price>(mid) - 1 - off : static_cast<Price>(mid) + 1 + off;
      out.push_back({Msg::Add, s, p, static_cast<Qty>(1 + rng() % 10), 0});
      adds.push_back(static_cast<std::uint32_t>(i));
    } else if (r < 93) {
      const std::size_t k = rng() % adds.size();
      out.push_back({Msg::Cancel, Side::Buy, 0, 0, adds[k]});
      adds[k] = adds.back();
      adds.pop_back();
    } else {
      out.push_back({Msg::Market, rng() % 2 ? Side::Buy : Side::Sell, 0, static_cast<Qty>(1 + rng() % 15), 0});
    }
  }
  return out;
}

struct Stats {
  double seconds;
  double p50, p99, p999, max;
};

double pct(std::vector<std::uint32_t>& v, double q) {
  const std::size_t k = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
  return v[k];
}

template <class Book, class Run>
Stats run(const std::vector<Msg>& msgs, Run&& step, bool timed) {
  std::vector<std::uint32_t> lat;
  if (timed) lat.reserve(msgs.size());
  const auto t0 = Clock::now();
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    if (timed) {
      const auto a = Clock::now();
      step(i);
      const auto b = Clock::now();
      lat.push_back(static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
    } else {
      step(i);
    }
  }
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  Stats s{secs, 0, 0, 0, 0};
  if (timed) {
    s.p50 = pct(lat, 0.5);
    s.p99 = pct(lat, 0.99);
    s.p999 = pct(lat, 0.999);
    s.max = *std::max_element(lat.begin(), lat.end());
  }
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t n = 5'000'000;
  bool json = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--json") == 0) json = true;
    else n = std::stoull(argv[i]);
  }
  const auto msgs = make_stream(n, 2026);

  auto fast_pass = [&](bool timed) {
    OrderBook book(0, 100000, n);
    std::vector<OrderId> ids(msgs.size(), kNoOrder);
    return run<OrderBook>(msgs, [&](std::size_t i) {
      const Msg& m = msgs[i];
      if (m.kind == Msg::Add) ids[i] = book.add_limit(m.side, m.price, m.qty).id;
      else if (m.kind == Msg::Cancel) book.cancel(ids[m.target]);
      else book.add_market(m.side, m.qty);
    }, timed);
  };
  auto ref_pass = [&](bool timed) {
    ReferenceBook book;
    std::vector<std::uint64_t> ids(msgs.size(), 0);
    return run<ReferenceBook>(msgs, [&](std::size_t i) {
      const Msg& m = msgs[i];
      if (m.kind == Msg::Add) ids[i] = book.add_limit(m.side, m.price, m.qty);
      else if (m.kind == Msg::Cancel) book.cancel(ids[m.target]);
      else book.add_limit(m.side, m.side == Side::Buy ? 1 << 30 : -(1 << 30), m.qty, true);
    }, timed);
  };

  // Best of 3 untimed passes for throughput; one timed pass for latency.
  Stats fast{1e9, 0, 0, 0, 0}, ref{1e9, 0, 0, 0, 0};
  for (int rep = 0; rep < 3; ++rep) {
    fast.seconds = std::min(fast.seconds, fast_pass(false).seconds);
    ref.seconds = std::min(ref.seconds, ref_pass(false).seconds);
  }
  const Stats fl = fast_pass(true), rl = ref_pass(true);
  const double fast_mps = static_cast<double>(n) / fast.seconds / 1e6;
  const double ref_mps = static_cast<double>(n) / ref.seconds / 1e6;

  if (json) {
    std::printf("{\"messages\": %zu, \"fast\": {\"msgs_per_sec_millions\": %.2f, \"ns_per_msg\": %.1f, "
                "\"p50_ns\": %.0f, \"p99_ns\": %.0f, \"p999_ns\": %.0f}, "
                "\"reference\": {\"msgs_per_sec_millions\": %.2f, \"ns_per_msg\": %.1f, "
                "\"p50_ns\": %.0f, \"p99_ns\": %.0f, \"p999_ns\": %.0f}, \"speedup\": %.2f}\n",
                n, fast_mps, 1e9 / (fast_mps * 1e6), fl.p50, fl.p99, fl.p999, ref_mps, 1e9 / (ref_mps * 1e6),
                rl.p50, rl.p99, rl.p999, fast_mps / ref_mps);
    return 0;
  }
  std::printf("tickforge order book benchmark: %zu messages (55%% add, 38%% cancel, 7%% market)\n\n", n);
  std::printf("%-22s %12s %10s %10s %10s %10s\n", "book", "M msgs/s", "ns/msg", "p50 ns", "p99 ns", "p99.9 ns");
  std::printf("%-22s %12.2f %10.1f %10.0f %10.0f %10.0f\n", "tickforge (fast)", fast_mps, 1e9 / (fast_mps * 1e6),
              fl.p50, fl.p99, fl.p999);
  std::printf("%-22s %12.2f %10.1f %10.0f %10.0f %10.0f\n", "std::map reference", ref_mps, 1e9 / (ref_mps * 1e6),
              rl.p50, rl.p99, rl.p999);
  std::printf("\nspeedup: %.1fx  (latency percentiles include ~20 ns of clock overhead)\n", fast_mps / ref_mps);
  return 0;
}
