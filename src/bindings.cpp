// Python bindings: the order book, the quoting rule and the simulator.
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "tickforge/market_sim.hpp"
#include "tickforge/order_book.hpp"

namespace py = pybind11;
using namespace tf;

namespace {

template <class T>
py::array_t<T> to_array(std::vector<T>&& v) {
  auto* heap = new std::vector<T>(std::move(v));
  py::capsule owner(heap, [](void* p) { delete static_cast<std::vector<T>*>(p); });
  return py::array_t<T>(static_cast<py::ssize_t>(heap->size()), heap->data(), owner);
}

py::dict result_to_dict(SimResult&& r) {
  py::dict d;
  d["t"] = to_array(std::move(r.t));
  d["mid"] = to_array(std::move(r.mid));
  d["fundamental"] = to_array(std::move(r.fundamental));
  d["spread"] = to_array(std::move(r.spread));
  d["inventory"] = to_array(std::move(r.inventory));
  d["pnl"] = to_array(std::move(r.pnl));
  d["maker_bid"] = to_array(std::move(r.maker_bid));
  d["maker_ask"] = to_array(std::move(r.maker_ask));
  d["fill_t"] = to_array(std::move(r.fill_t));
  d["fill_price"] = to_array(std::move(r.fill_price));
  d["fill_mid"] = to_array(std::move(r.fill_mid));
  d["fill_fundamental"] = to_array(std::move(r.fill_fundamental));
  d["fill_side"] = to_array(std::move(r.fill_side));
  d["fill_taker"] = to_array(std::move(r.fill_taker));
  d["fill_qty"] = to_array(std::move(r.fill_qty));
  d["trade_t"] = to_array(std::move(r.trade_t));
  d["trade_price"] = to_array(std::move(r.trade_price));
  d["trade_mid_before"] = to_array(std::move(r.trade_mid_before));
  d["trade_side"] = to_array(std::move(r.trade_side));
  d["trade_taker"] = to_array(std::move(r.trade_taker));
  d["trade_maker"] = to_array(std::move(r.trade_maker));
  d["trade_fundamental"] = to_array(std::move(r.trade_fundamental));
  d["trade_qty"] = to_array(std::move(r.trade_qty));
  d["events"] = r.events;
  d["maker_orders"] = r.maker_orders;
  d["maker_cancels"] = r.maker_cancels;
  d["final_cash"] = r.final_cash;
  d["final_mid"] = r.final_mid;
  d["final_fundamental"] = r.final_fundamental;
  d["final_inventory"] = r.final_inventory;
  return d;
}

}  // namespace

PYBIND11_MODULE(_core, m) {
  m.doc() = "tickforge C++ core: matching engine and market simulator";

  py::enum_<Side>(m, "Side").value("BUY", Side::Buy).value("SELL", Side::Sell);
  py::enum_<Tif>(m, "Tif").value("GTC", Tif::GTC).value("IOC", Tif::IOC);
  py::enum_<Strategy>(m, "Strategy")
      .value("NONE", Strategy::None)
      .value("SYMMETRIC", Strategy::Symmetric)
      .value("GLFT", Strategy::Glft);

  py::class_<Fill>(m, "Fill")
      .def_readonly("maker_id", &Fill::maker_id)
      .def_readonly("taker_id", &Fill::taker_id)
      .def_readonly("maker_owner", &Fill::maker_owner)
      .def_readonly("taker_owner", &Fill::taker_owner)
      .def_readonly("price", &Fill::price)
      .def_readonly("qty", &Fill::qty)
      .def_readonly("taker_side", &Fill::taker_side);

  py::class_<AddResult>(m, "AddResult")
      .def_readonly("id", &AddResult::id)
      .def_readonly("filled", &AddResult::filled)
      .def_readonly("resting", &AddResult::resting)
      .def_readonly("rejected", &AddResult::rejected);

  py::class_<OrderBook>(m, "OrderBook")
      .def(py::init<Price, Price, std::size_t>(), py::arg("min_price"), py::arg("max_price"),
           py::arg("reserve_orders") = 1 << 16)
      .def("add_limit", &OrderBook::add_limit, py::arg("side"), py::arg("price"), py::arg("qty"),
           py::arg("owner") = 0, py::arg("tif") = Tif::GTC)
      .def("add_market", &OrderBook::add_market, py::arg("side"), py::arg("qty"), py::arg("owner") = 0)
      .def("cancel", &OrderBook::cancel)
      .def("reduce", &OrderBook::reduce)
      .def("has_bid", &OrderBook::has_bid)
      .def("has_ask", &OrderBook::has_ask)
      .def("best_bid", &OrderBook::best_bid)
      .def("best_ask", &OrderBook::best_ask)
      .def("depth", &OrderBook::depth)
      .def("is_live", &OrderBook::is_live)
      .def("order_qty", &OrderBook::order_qty)
      .def("queue_ahead", &OrderBook::queue_ahead)
      .def("live_orders", &OrderBook::live_orders)
      .def("fills", &OrderBook::fills)
      .def("levels", [](const OrderBook& b, Side s, std::size_t n) {
        py::list out;
        for (const auto& lv : b.levels(s, n)) out.append(py::make_tuple(lv.price, lv.qty, lv.orders));
        return out;
      });

#define FIELD(cls, name) .def_readwrite(#name, &cls::name)
  py::class_<MarketConfig>(m, "MarketConfig")
      .def(py::init<>())
      FIELD(MarketConfig, duration) FIELD(MarketConfig, warmup) FIELD(MarketConfig, start_price)
      FIELD(MarketConfig, price_band) FIELD(MarketConfig, lp_rate) FIELD(MarketConfig, lp_cancel_rate)
      FIELD(MarketConfig, lp_offset_p) FIELD(MarketConfig, lp_max_size) FIELD(MarketConfig, noise_rate)
      FIELD(MarketConfig, noise_max_size) FIELD(MarketConfig, informed_rate)
      FIELD(MarketConfig, informed_max_size) FIELD(MarketConfig, jump_rate)
      FIELD(MarketConfig, sample_interval);

  py::class_<MakerConfig>(m, "MakerConfig")
      .def(py::init<>())
      FIELD(MakerConfig, strategy) FIELD(MakerConfig, update_interval) FIELD(MakerConfig, latency)
      FIELD(MakerConfig, quote_size) FIELD(MakerConfig, max_inventory) FIELD(MakerConfig, half_spread)
      FIELD(MakerConfig, gamma) FIELD(MakerConfig, kappa) FIELD(MakerConfig, A) FIELD(MakerConfig, sigma)
      FIELD(MakerConfig, imbalance_coef) FIELD(MakerConfig, lead_coef);
#undef FIELD

  py::class_<Quotes>(m, "Quotes")
      .def_readonly("has_bid", &Quotes::has_bid)
      .def_readonly("has_ask", &Quotes::has_ask)
      .def_readonly("bid", &Quotes::bid)
      .def_readonly("ask", &Quotes::ask);

  m.def(
      "compute_quotes",
      [](const MakerConfig& c, bool has_bid, bool has_ask, Price bb, Price ba, Qty bq, Qty aq, double mid,
         Qty inventory, double lead) {
        return compute_quotes(c, BookState{has_bid, has_ask, bb, ba, bq, aq, mid, lead}, inventory);
      },
      py::arg("maker"), py::arg("has_bid"), py::arg("has_ask"), py::arg("best_bid"), py::arg("best_ask"),
      py::arg("bid_qty"), py::arg("ask_qty"), py::arg("mid"), py::arg("inventory"), py::arg("lead") = 0.0);

  m.def(
      "simulate",
      [](const MarketConfig& market, const MakerConfig& maker, std::uint64_t seed) {
        SimResult r;
        {
          py::gil_scoped_release release;  // lets Python threads run simulations in parallel
          r = simulate(market, maker, seed);
        }
        return result_to_dict(std::move(r));
      },
      py::arg("market"), py::arg("maker"), py::arg("seed"));
}
