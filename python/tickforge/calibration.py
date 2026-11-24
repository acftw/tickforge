"""Calibrate the GLFT model's inputs from market data the maker could observe.

The closed-form quotes need three numbers:

* sigma: volatility of the mid price (ticks per square-root second);
* A, kappa: the fill-intensity curve lambda(delta) = A * exp(-kappa * delta),
  i.e. how often a quote at distance delta from the mid would be reached.

We estimate them from simulated sessions with no market maker, using only the
public tape (trade prices and the mid just before each trade), the same way you
would from exchange data. lambda(delta) is the rate of trades printing at least
delta away from the mid, per side.

Note what this estimate assumes: a quote is filled whenever a trade reaches its
price. In a real queue you are only filled after everyone ahead of you, so the
true fill rate of a new quote is lower. The study measures that gap.
"""

from __future__ import annotations

import numpy as np

from tickforge._core import MakerConfig, MarketConfig, simulate

DELTAS = np.array([0.5, 1.0, 1.5, 2.0, 2.5, 3.0])


def estimate_sigma(t: np.ndarray, mid: np.ndarray, step_seconds: float = 1.0) -> float:
    """Mid-price volatility in ticks per sqrt(second), from returns over `step_seconds`."""
    dt = float(t[1] - t[0])
    stride = max(1, int(round(step_seconds / dt)))
    changes = np.diff(mid[::stride])
    return float(changes.std(ddof=1) / np.sqrt(stride * dt))


def trade_through_rates(price, mid_before, duration: float, deltas=DELTAS) -> np.ndarray:
    """Trades per second, per side, printing at least delta ticks from the mid."""
    dist = np.abs(np.asarray(price) - np.asarray(mid_before))
    return np.array([(dist >= d - 1e-9).sum() / 2.0 / duration for d in deltas])


def fit_intensity(deltas: np.ndarray, rates: np.ndarray) -> tuple[float, float]:
    """Fit lambda(delta) = A exp(-kappa delta) by least squares on log rates.

    Points are weighted by sqrt(count) because rates estimated from few trades
    are noisy.
    """
    ok = rates > 0
    if ok.sum() < 2:
        raise ValueError("need at least two non-zero rates to fit the intensity curve")
    w = np.sqrt(rates[ok])
    slope, intercept = np.polyfit(deltas[ok], np.log(rates[ok]), 1, w=w)
    return float(np.exp(intercept)), float(-slope)


def calibrate(market: MarketConfig, seeds) -> dict:
    """Estimate sigma, A and kappa from market-maker-free sessions."""
    sig, rates = [], []
    for s in seeds:
        r = simulate(market, MakerConfig(), int(s))
        sig.append(estimate_sigma(r["t"], r["mid"]))
        rates.append(trade_through_rates(r["trade_price"], r["trade_mid_before"], market.duration))
    rates = np.mean(rates, axis=0)
    A, kappa = fit_intensity(DELTAS, rates)
    return {
        "sigma": float(np.mean(sig)),
        "A": A,
        "kappa": kappa,
        "deltas": DELTAS.tolist(),
        "trade_through_rates": rates.tolist(),
        "seeds": [int(s) for s in seeds],
    }
