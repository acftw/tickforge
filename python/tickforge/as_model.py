"""Replication of Avellaneda & Stoikov (2008), "High-frequency trading in a limit order book".

The paper's world: the mid price is a random walk, and a quote at distance delta
from the mid is filled with probability A * exp(-k * delta) * dt in each small
time step. The market maker chooses where to quote to maximise exponential
utility of terminal wealth. The closed-form answer is

    reservation price  r = s - q * gamma * sigma^2 * (T - t)
    total spread         = gamma * sigma^2 * (T - t) + (2 / gamma) * ln(1 + gamma / k)

quoted symmetrically around r. The paper compares this "inventory strategy"
with a "symmetric strategy" that uses the same average spread but centres it
on the mid price, over 1000 simulations, and reports the results in Tables 1-3.

This module reruns that experiment so the numbers can be checked against the
published tables (`PAPER_TABLES`).
"""

from __future__ import annotations

from dataclasses import asdict, dataclass

import numpy as np

# Published results (Avellaneda & Stoikov 2008, Section 3.3, Tables 1-3).
# s=100, T=1, sigma=2, dt=0.005, q0=0, k=1.5, A=140, 1000 simulations.
PAPER_TABLES = {
    0.1: {
        "inventory": {"spread": 1.49, "profit": 65.0, "std_profit": 6.6, "final_q": 0.08, "std_final_q": 2.9},
        "symmetric": {"spread": 1.49, "profit": 68.4, "std_profit": 12.7, "final_q": 0.26, "std_final_q": 8.4},
    },
    0.01: {
        "inventory": {"spread": 1.35, "profit": 68.6, "std_profit": 8.7, "final_q": 0.12, "std_final_q": 5.1},
        "symmetric": {"spread": 1.35, "profit": 68.8, "std_profit": 12.8, "final_q": 0.09, "std_final_q": 8.7},
    },
    1.0: {
        "inventory": {"spread": 3.02, "profit": 31.4, "std_profit": 5.0, "final_q": 0.02, "std_final_q": 1.7},
        "symmetric": {"spread": 3.02, "profit": 44.0, "std_profit": 11.0, "final_q": 0.00, "std_final_q": 5.1},
    },
}


@dataclass(frozen=True)
class ASParams:
    s0: float = 100.0
    T: float = 1.0
    sigma: float = 2.0
    dt: float = 0.005
    k: float = 1.5
    A: float = 140.0


def optimal_spread(gamma: float, tau: np.ndarray | float, p: ASParams) -> np.ndarray | float:
    """Total bid-ask spread of the inventory strategy with time-to-horizon tau."""
    return gamma * p.sigma**2 * tau + (2.0 / gamma) * np.log1p(gamma / p.k)


def reservation_price(s, q, gamma: float, tau, p: ASParams):
    return s - q * gamma * p.sigma**2 * tau


def average_spread(gamma: float, p: ASParams = ASParams()) -> float:
    """Time-average of the optimal spread over [0, T] (the symmetric strategy's spread)."""
    n = int(round(p.T / p.dt))
    tau = p.T - np.arange(n) * p.dt
    return float(np.mean(optimal_spread(gamma, tau, p)))


def simulate(gamma: float, n_paths: int = 1000, seed: int = 0, p: ASParams = ASParams()) -> dict:
    """Run both strategies on the same random price paths and fill draws.

    Returns a dict with per-strategy summary statistics in the paper's format
    plus the raw terminal profits and inventories.
    """
    rng = np.random.default_rng(seed)
    n = int(round(p.T / p.dt))
    sym_spread = average_spread(gamma, p)

    # Common random numbers: both strategies see the same price moves and the
    # same uniform draws for fills, so their difference is mostly strategy.
    steps = np.where(rng.random((n, n_paths)) < 0.5, 1.0, -1.0) * p.sigma * np.sqrt(p.dt)
    u_bid = rng.random((n, n_paths))
    u_ask = rng.random((n, n_paths))

    out = {}
    for name in ("inventory", "symmetric"):
        s = np.full(n_paths, p.s0)
        q = np.zeros(n_paths)
        cash = np.zeros(n_paths)
        spreads = np.empty(n)
        for i in range(n):
            tau = p.T - i * p.dt
            if name == "inventory":
                spread = optimal_spread(gamma, tau, p)
                centre = reservation_price(s, q, gamma, tau, p)
            else:
                spread = sym_spread
                centre = s
            spreads[i] = spread
            bid = centre - spread / 2.0
            ask = centre + spread / 2.0
            d_bid = s - bid
            d_ask = ask - s
            p_bid = np.clip(p.A * np.exp(-p.k * d_bid) * p.dt, 0.0, 1.0)
            p_ask = np.clip(p.A * np.exp(-p.k * d_ask) * p.dt, 0.0, 1.0)
            hit_bid = u_bid[i] < p_bid
            hit_ask = u_ask[i] < p_ask
            q += hit_bid.astype(float) - hit_ask.astype(float)
            cash += np.where(hit_ask, ask, 0.0) - np.where(hit_bid, bid, 0.0)
            s = s + steps[i]
        profit = cash + q * s
        out[name] = {
            "spread": float(np.mean(spreads)),
            "profit": float(profit.mean()),
            "std_profit": float(profit.std(ddof=1)),
            "final_q": float(q.mean()),
            "std_final_q": float(q.std(ddof=1)),
            "profits": profit,
            "final_inventory": q,
        }
    return {"gamma": gamma, "n_paths": n_paths, "params": asdict(p), **out}


def replicate(n_paths: int = 1000, seed: int = 2008, gammas=(0.1, 0.01, 1.0)) -> list[dict]:
    """Rerun Tables 1-3 and compare with the published numbers.

    For each statistic we also report how many standard errors our estimate is
    from the paper's (for means) so differences can be judged against Monte
    Carlo noise rather than by eye.
    """
    rows = []
    for g in gammas:
        res = simulate(g, n_paths=n_paths, seed=seed)
        for strat in ("inventory", "symmetric"):
            ours, paper = res[strat], PAPER_TABLES[g][strat]
            se_profit = ours["std_profit"] / np.sqrt(n_paths)
            rows.append(
                {
                    "gamma": g,
                    "strategy": strat,
                    "spread": round(ours["spread"], 2),
                    "paper_spread": paper["spread"],
                    "profit": round(ours["profit"], 2),
                    "paper_profit": paper["profit"],
                    "profit_se": round(float(se_profit), 2),
                    "std_profit": round(ours["std_profit"], 2),
                    "paper_std_profit": paper["std_profit"],
                    "final_q": round(ours["final_q"], 2),
                    "paper_final_q": paper["final_q"],
                    "std_final_q": round(ours["std_final_q"], 2),
                    "paper_std_final_q": paper["std_final_q"],
                }
            )
    return rows
