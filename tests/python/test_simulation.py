import numpy as np
import tickforge as tf
from tickforge.analysis import bootstrap_ci, markouts, paired_difference, session_metrics


def glft(**kw):
    k = tf.MakerConfig()
    k.strategy = tf.Strategy.GLFT
    k.gamma, k.kappa, k.A, k.sigma = 50.0, 5.0, 24.0, 0.25
    for a, v in kw.items():
        setattr(k, a, v)
    return k


def market(duration=900.0):
    m = tf.MarketConfig()
    m.duration = duration
    return m


def test_accounting_identities():
    r = tf.simulate(market(), glft(latency=0.02), 7)
    side, qty, px = r["fill_side"], r["fill_qty"], r["fill_price"]
    assert (side * qty).sum() == r["final_inventory"]
    assert np.isclose(-(side * px * qty).sum(), r["final_cash"])
    assert np.all(np.abs(r["inventory"]) <= 20)


def test_maker_quotes_never_cross_the_book():
    r = tf.simulate(market(), glft(), 3)
    both = ~np.isnan(r["maker_bid"]) & ~np.isnan(r["maker_ask"])
    assert np.all(r["maker_bid"][both] < r["maker_ask"][both])


def test_deterministic_and_seed_dependent():
    a = tf.simulate(market(300), glft(), 1)
    b = tf.simulate(market(300), glft(), 1)
    c = tf.simulate(market(300), glft(), 2)
    assert a["final_cash"] == b["final_cash"]
    assert not np.array_equal(a["mid"], c["mid"])


def test_informed_traders_only_buy_below_value():
    r = tf.simulate(market(), tf.MakerConfig(), 5)
    inf = r["trade_taker"] == 4
    gain = (r["trade_fundamental"][inf] - r["trade_price"][inf]) * r["trade_side"][inf]
    assert np.all(gain >= 1)


def test_lead_signal_reduces_adverse_selection():
    m = market()
    plain = [session_metrics(tf.simulate(m, glft(latency=0.02), s)) for s in range(4)]
    lead = [session_metrics(tf.simulate(m, glft(latency=0.02, lead_coef=1.0), s)) for s in range(4)]
    assert np.mean([x["informed_share"] for x in lead]) < np.mean([x["informed_share"] for x in plain])


def test_markout_math_on_toy_data():
    r = {
        "t": np.array([0.0, 1.0, 2.0]),
        "mid": np.array([100.0, 101.0, 102.0]),
        "fill_t": np.array([0.0]),
        "fill_price": np.array([99.5]),
        "fill_side": np.array([1]),
        "fill_qty": np.array([2]),
    }
    mk = markouts(r, horizons=(1.0, 2.0))
    assert mk == {1.0: 1.5, 2.0: 2.5}


def test_bootstrap_and_paired():
    x = np.random.default_rng(0).normal(5, 1, 500)
    lo, hi = bootstrap_ci(x)
    assert lo < 5 < hi
    d = paired_difference([{"pnl": 2.0}] * 10, [{"pnl": 1.0}] * 10)
    assert d["mean"] == 1.0 and d["share_positive"] == 1.0
