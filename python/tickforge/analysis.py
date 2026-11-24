"""Performance and microstructure metrics for simulated market-making sessions.

All prices are in ticks. One session is one simulated hour, so a session's
final P&L is "P&L per hour".
"""

from __future__ import annotations

import numpy as np

INFORMED = 4
MARKOUT_HORIZONS = (0.1, 0.5, 1.0, 5.0, 10.0, 30.0, 60.0)


def mid_at(t_grid: np.ndarray, mid: np.ndarray, times: np.ndarray) -> np.ndarray:
    """Mid price at the first grid point at or after each time (clipped to the end)."""
    idx = np.searchsorted(t_grid, times, side="left")
    return mid[np.clip(idx, 0, len(mid) - 1)]


def markouts(r: dict, horizons=MARKOUT_HORIZONS) -> dict[float, float]:
    """Average gain per lot, measured at the mid `h` seconds after each fill.

    markout(h) = side * (mid(t + h) - fill price), averaged over lots. At h = 0
    this is the spread captured. As h grows, it shows how much of that spread
    is given back because prices move against the maker after it trades
    (adverse selection).
    """
    lots = r["fill_qty"].astype(float)
    if lots.sum() == 0:
        return {h: float("nan") for h in horizons}
    out = {}
    for h in horizons:
        m = mid_at(r["t"], r["mid"], r["fill_t"] + h)
        out[h] = float(np.sum(r["fill_side"] * (m - r["fill_price"]) * lots) / lots.sum())
    return out


def session_metrics(r: dict, horizons=MARKOUT_HORIZONS) -> dict:
    lots = r["fill_qty"].astype(float)
    total_lots = float(lots.sum())
    pnl = float(r["final_cash"] + r["final_inventory"] * r["final_mid"])
    capture = (
        float(np.sum(r["fill_side"] * (r["fill_mid"] - r["fill_price"]) * lots) / total_lots)
        if total_lots
        else float("nan")
    )
    informed_share = float(lots[r["fill_taker"] == INFORMED].sum() / total_lots) if total_lots else float("nan")
    # P&L path sampled once a minute
    t, path = r["t"], r["pnl"]
    stride = max(1, int(round(60.0 / (t[1] - t[0]))))
    minute = np.diff(path[::stride])
    return {
        "pnl": pnl,
        "lots": total_lots,
        "fills": len(lots),
        "capture_per_lot": capture,
        "informed_share": informed_share,
        "inventory_std": float(np.std(r["inventory"])),
        "inventory_max": float(np.max(np.abs(r["inventory"]))),
        "pnl_vol_per_minute": float(minute.std(ddof=1)) if len(minute) > 1 else float("nan"),
        "markouts": markouts(r, horizons),
        "final_inventory": int(r["final_inventory"]),
        "maker_orders": int(r["maker_orders"]),
    }


def bootstrap_ci(x, n_boot: int = 4000, alpha: float = 0.05, seed: int = 0) -> tuple[float, float]:
    """Percentile bootstrap confidence interval for the mean."""
    x = np.asarray(x, dtype=float)
    rng = np.random.default_rng(seed)
    means = x[rng.integers(0, len(x), size=(n_boot, len(x)))].mean(axis=1)
    return float(np.quantile(means, alpha / 2)), float(np.quantile(means, 1 - alpha / 2))


def summarize(sessions: list[dict]) -> dict:
    """Aggregate per-session metrics into the numbers reported in the study."""
    pnl = np.array([s["pnl"] for s in sessions])
    lo, hi = bootstrap_ci(pnl)
    horizons = list(sessions[0]["markouts"].keys())
    lots = np.array([s["lots"] for s in sessions])

    def lot_weighted(key):
        v = np.array([s[key] for s in sessions])
        ok = lots > 0
        return float(np.sum(v[ok] * lots[ok]) / lots[ok].sum())

    mk = {
        str(h): float(np.sum([s["markouts"][h] * s["lots"] for s in sessions if s["lots"] > 0]) / lots[lots > 0].sum())
        for h in horizons
    }
    return {
        "sessions": len(sessions),
        "pnl_mean": float(pnl.mean()),
        "pnl_ci95": [lo, hi],
        "pnl_std": float(pnl.std(ddof=1)),
        "sharpe_per_hour": float(pnl.mean() / pnl.std(ddof=1)) if pnl.std() > 0 else float("nan"),
        "loss_sessions": int((pnl < 0).sum()),
        "lots_per_hour": float(lots.mean()),
        "capture_per_lot": lot_weighted("capture_per_lot"),
        "informed_share": lot_weighted("informed_share"),
        "inventory_std": float(np.mean([s["inventory_std"] for s in sessions])),
        "pnl_vol_per_minute": float(np.mean([s["pnl_vol_per_minute"] for s in sessions])),
        "markouts": mk,
    }


def paired_difference(a: list[dict], b: list[dict], key: str = "pnl") -> dict:
    """Mean of a - b over sessions that share seeds, with a bootstrap 95% CI."""
    d = np.array([x[key] - y[key] for x, y in zip(a, b)])
    lo, hi = bootstrap_ci(d)
    return {"mean": float(d.mean()), "ci95": [lo, hi], "share_positive": float((d > 0).mean())}
