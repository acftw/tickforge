"""Data for the project website (docs/index.html).

The page source is site/page.html (also published as a hosted artifact, which adds
the document wrapper itself). This module writes a complete docs/index.html for
GitHub Pages from it, plus two JSON files next to the page:

* data/landscape.json: one five-minute example session replayed twice on the
  same random seed, once with the symmetric maker and once with GLFT + lead
  signal, recording the full order book depth every half second. This drives
  the 3D order-book model.
* data/summary.json: the numbers behind the charts, copied from results/.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from tickforge._core import MarketConfig, simulate
from tickforge.study import maker_config

SHOWCASE = {"symmetric": "Symmetric (join the touch)", "glft+lead": "GLFT + lead signal"}
INFORMED = 4


def _clean(a, digits=2):
    return [None if not np.isfinite(x) else round(float(x), digits) for x in a]


def landscape(study: dict, seed: int = 17, duration: float = 300.0, dt: float = 0.5, window: int = 30) -> dict:
    calib, gamma = study["calibration"], study["tuning"]["best_gamma"]
    imb = study["tuning"]["best_imbalance"]
    latency = study["config"]["base_latency"]
    m = MarketConfig()
    m.duration, m.sample_interval, m.depth_window = duration, dt, window
    raw = {k: simulate(m, maker_config(k, calib, gamma, latency, imb), seed) for k in SHOWCASE}

    # Crop the price axis to the band the session actually used.
    lo, hi = m.start_price + window, m.start_price - window
    for r in raw.values():
        grid = r["depth"].reshape(len(r["t"]), 2 * window + 1)
        used = np.nonzero(np.abs(grid).sum(axis=0) > 0)[0]
        lo = min(lo, m.start_price - window + int(used.min()))
        hi = max(hi, m.start_price - window + int(used.max()))
    lo = max(lo, int(min(r["fundamental"].min() for r in raw.values())) - 6)
    hi = min(hi, int(max(r["fundamental"].max() for r in raw.values())) + 6)
    c0, c1 = lo - (m.start_price - window), hi - (m.start_price - window) + 1

    runs = {}
    for k, r in raw.items():
        n = len(r["t"])
        grid = r["depth"].reshape(n, 2 * window + 1)[:, c0:c1]
        idx = lambda t, n=n: np.clip(np.floor(np.asarray(t) / dt).astype(int), 0, n - 1)  # noqa: E731
        inf = r["trade_taker"] == INFORMED
        sweeps = {}
        for ti, px, q in zip(idx(r["trade_t"][inf]), r["trade_price"][inf], r["trade_qty"][inf]):
            key = (int(ti), int(px))
            sweeps[key] = sweeps.get(key, 0) + int(q)
        fills = [
            [int(ti), int(px), int(sd), int(tk == INFORMED), int(q)]
            for ti, px, sd, tk, q in zip(
                idx(r["fill_t"]), r["fill_price"], r["fill_side"], r["fill_taker"], r["fill_qty"]
            )
        ]
        lots = float(r["fill_qty"].sum())
        runs[k] = {
            "label": SHOWCASE[k],
            "depth": grid.astype(int).ravel().tolist(),
            "mid": _clean(r["mid"]),
            "fundamental": _clean(r["fundamental"], 0),
            "bid": _clean(r["maker_bid"], 0),
            "ask": _clean(r["maker_ask"], 0),
            "inventory": _clean(r["inventory"], 0),
            "pnl": _clean(r["pnl"] - r["pnl"][0], 1),
            "sweeps": [[t, p, q] for (t, p), q in sorted(sweeps.items())],
            "fills": fills,
            "totals": {
                "pnl": round(float(r["final_cash"] + r["final_inventory"] * r["final_mid"]), 1),
                "lots": int(lots),
                "informed_lots": int(r["fill_qty"][r["fill_taker"] == INFORMED].sum()),
            },
        }
    jumps = int(np.count_nonzero(np.diff(raw["symmetric"]["fundamental"])))
    return {
        "meta": {
            "seed": seed,
            "duration": duration,
            "dt": dt,
            "samples": len(raw["symmetric"]["t"]),
            "price_lo": lo,
            "price_hi": hi,
            "latency_ms": latency * 1000,
            "gamma": gamma,
            "jumps": jumps,
        },
        "runs": runs,
    }


def summary(study: dict, replication: dict, benchmark: dict) -> dict:
    keep = (
        "pnl_mean",
        "pnl_ci95",
        "pnl_std",
        "sharpe_per_hour",
        "loss_sessions",
        "lots_per_hour",
        "capture_per_lot",
        "informed_share",
        "inventory_std",
        "pnl_vol_per_minute",
        "markouts",
        "sessions",
    )
    return {
        "comparison": {k: {f: v[f] for f in keep} for k, v in study["comparison"].items()},
        "paired": study["paired"],
        "queue_haircut": study["queue_haircut"],
        "latency_sweep": study["latency_sweep"],
        "breakeven_latency_ms": study["breakeven_latency_ms"],
        "frontier": study["tuning"]["gamma_frontier"],
        "best_gamma": study["tuning"]["best_gamma"],
        "calibration": {k: study["calibration"][k] for k in ("sigma", "A", "kappa")},
        "base_latency_ms": study["config"]["base_latency"] * 1000,
        "test_sessions": len(study["config"]["test_seeds"]),
        "replication": replication["rows"],
        "benchmark": benchmark,
    }


HEAD = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<style>:root{padding-top:env(safe-area-inset-top,0px);padding-bottom:env(safe-area-inset-bottom,0px)}
body{margin:0}img{max-width:100%}[hidden]{display:none!important}</style>
"""


def build(results: Path, out: Path, seed: int = 17, page: Path | None = None) -> list[Path]:
    """Write the site's data files and, if the page source exists, a complete docs/index.html."""
    results, out = Path(results), Path(out)
    page = Path(page) if page else results.parent / "site" / "page.html"
    study = json.loads((results / "study.json").read_text())
    rep = json.loads((results / "as_replication.json").read_text())
    bench = json.loads((results / "benchmark.json").read_text())
    (out / "data").mkdir(parents=True, exist_ok=True)
    paths = [out / "data" / "landscape.json", out / "data" / "summary.json"]
    paths[0].write_text(json.dumps(landscape(study, seed=seed), separators=(",", ":")))
    paths[1].write_text(json.dumps(summary(study, rep, bench), separators=(",", ":")))
    if page.exists():
        paths.append(out / "index.html")
        src = page.read_text()
        cut = src.index("</style>") + len("</style>")  # title, meta, fonts and styles go in <head>
        paths[-1].write_text(HEAD + src[:cut] + "\n</head>\n<body>\n" + src[cut:] + "\n</body>\n</html>\n")
    return paths
