import numpy as np
from tickforge.as_model import PAPER_TABLES, ASParams, average_spread, optimal_spread, replicate, simulate


def test_average_spread_matches_paper():
    for gamma, table in PAPER_TABLES.items():
        assert abs(average_spread(gamma) - table["inventory"]["spread"]) < 0.015


def test_spread_formula():
    p = ASParams()
    assert np.isclose(optimal_spread(0.1, 1.0, p), 0.1 * 4 + 20 * np.log(1 + 0.1 / 1.5))


def test_inventory_strategy_cuts_risk():
    res = simulate(0.1, n_paths=400, seed=1)
    assert res["inventory"]["std_profit"] < 0.7 * res["symmetric"]["std_profit"]
    assert res["inventory"]["std_final_q"] < 0.5 * res["symmetric"]["std_final_q"]


def test_replication_within_monte_carlo_error():
    for row in replicate(n_paths=1000, seed=2008):
        z = (row["profit"] - row["paper_profit"]) / row["profit_se"]
        assert abs(z) < 3, row
        assert abs(row["std_profit"] / row["paper_std_profit"] - 1) < 0.1, row
