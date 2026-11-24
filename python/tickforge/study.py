"""The market-making study: calibrate, tune on training sessions, test out of sample.

Every number in the README comes from `run_study`. The protocol mirrors how
you would test a strategy honestly on real data:

1. Calibrate sigma, A and kappa from sessions with no market maker.
2. Choose the free parameters (GLFT risk aversion, imbalance weight) on
   *training* sessions only, by the highest mean P&L. Risk is controlled by
   the inventory limit; selecting by Sharpe instead pushes gamma to extreme
   values that give up a third of the P&L in sessions that never lose anyway.
3. Report every strategy on separate *test* sessions it has never seen, with
   bootstrap confidence intervals and paired comparisons (all strategies face
   the same random seeds).
4. Sweep the maker's latency to measure how fast each strategy must be to
   make money, and how much latency the lead signal lets you tolerate.
"""

from __future__ import annotations

import os
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict, dataclass, field

import numpy as np

from tickforge._core import MakerConfig, MarketConfig, Strategy, simulate
from tickforge.analysis import paired_difference, session_metrics, summarize
from tickforge.calibration import calibrate


@dataclass
class StudyConfig:
    duration: float = 3600.0
    calibration_seeds: list = field(default_factory=lambda: list(range(10)))
    train_seeds: list = field(default_factory=lambda: list(range(1000, 1040)))
    test_seeds: list = field(default_factory=lambda: list(range(2000, 2200)))
    latency_seeds: list = field(default_factory=lambda: list(range(3000, 3100)))
    base_latency: float = 0.020
    gammas: tuple = (2.0, 5.0, 10.0, 20.0, 30.0, 50.0, 80.0, 120.0, 200.0, 300.0, 500.0)
    imbalance_coefs: tuple = (0.0, 0.25, 0.5, 1.0, 1.5)
    latencies_ms: tuple = (0, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000)
    workers: int = field(default_factory=lambda: os.cpu_count() or 1)

    @classmethod
    def quick(cls) -> StudyConfig:
        """A small version for CI and smoke tests (minutes -> seconds)."""
        return cls(
            duration=600.0,
            calibration_seeds=list(range(3)),
            train_seeds=list(range(1000, 1006)),
            test_seeds=list(range(2000, 2012)),
            latency_seeds=list(range(3000, 3008)),
            gammas=(2.0, 20.0),
            imbalance_coefs=(0.0, 0.5),
            latencies_ms=(0, 20, 500),
        )


def market_config(duration: float) -> MarketConfig:
    m = MarketConfig()
    m.duration = duration
    return m


def maker_config(
    kind: str, calib: dict, gamma: float = 20.0, latency: float = 0.0, imbalance: float = 0.0
) -> MakerConfig:
    """Build one of the strategies compared in the study.

    kind: "symmetric" (join the touch, inventory limit only), "glft",
    "glft+imbalance" (fair value nudged by queue imbalance), "glft+lead" (fair
    value = the lead signal).
    """
    k = MakerConfig()
    k.latency = latency
    if kind == "symmetric":
        k.strategy = Strategy.SYMMETRIC
        k.half_spread = 0.5
        return k
    k.strategy = Strategy.GLFT
    k.gamma = gamma
    k.sigma = calib["sigma"]
    k.A = calib["A"]
    k.kappa = calib["kappa"]
    if kind == "glft+imbalance":
        k.imbalance_coef = imbalance
    elif kind == "glft+lead":
        k.lead_coef = 1.0
    elif kind != "glft":
        raise ValueError(f"unknown strategy {kind!r}")
    return k


def run_sessions(market: MarketConfig, maker: MakerConfig, seeds, workers: int, keep_raw: bool = False):
    """Simulate one session per seed (in parallel; the C++ core releases the GIL)."""

    def one(seed):
        r = simulate(market, maker, int(seed))
        m = session_metrics(r)
        m["seed"] = int(seed)
        if keep_raw:
            m["_raw"] = r
        return m

    with ThreadPoolExecutor(max_workers=workers) as ex:
        return list(ex.map(one, seeds))


def queue_haircut(sessions_raw: list[dict], calib: dict) -> dict:
    """Fills the textbook intensity model predicts for the maker's quotes vs fills it actually got.

    For each sampled instant we take the distance of each live quote from the
    mid and add lambda(delta) * dt using the calibrated curve. A model that
    ignored queues would expect that many fills; the queue decides how many
    the maker really receives.
    """
    A, kappa = calib["A"], calib["kappa"]
    predicted = realized = 0.0
    for r in sessions_raw:
        dt = float(r["t"][1] - r["t"][0])
        for quote, sign in ((r["maker_bid"], 1.0), (r["maker_ask"], -1.0)):
            live = ~np.isnan(quote)
            delta = np.maximum(sign * (r["mid"][live] - quote[live]), 0.0)
            predicted += float(np.sum(A * np.exp(-kappa * delta)) * dt)
        realized += len(r["fill_t"])
    return {
        "predicted_fills": predicted,
        "realized_fills": realized,
        "ratio": realized / predicted if predicted else float("nan"),
    }


def run_study(cfg: StudyConfig | None = None, progress=print) -> dict:
    cfg = cfg or StudyConfig()
    t0 = time.time()
    market = market_config(cfg.duration)
    W = cfg.workers

    progress("calibrating sigma, A, kappa from market-maker-free sessions ...")
    calib = calibrate(market, cfg.calibration_seeds)
    progress(f"  sigma={calib['sigma']:.3f} ticks/sqrt(s)  A={calib['A']:.2f}/s  kappa={calib['kappa']:.2f}/tick")

    # --- 1. tune on training sessions ----------------------------------------
    progress("tuning GLFT risk aversion on training sessions ...")
    frontier = []
    for g in cfg.gammas:
        s = summarize(run_sessions(market, maker_config("glft", calib, g, cfg.base_latency), cfg.train_seeds, W))
        frontier.append(
            {
                "gamma": g,
                **{
                    k: s[k]
                    for k in (
                        "pnl_mean",
                        "pnl_std",
                        "sharpe_per_hour",
                        "pnl_vol_per_minute",
                        "inventory_std",
                        "lots_per_hour",
                    )
                },
            }
        )
        progress(
            f"  gamma={g:<5} pnl/h={s['pnl_mean']:8.1f}  sharpe/h={s['sharpe_per_hour']:6.2f}  "
            f"inv sd={s['inventory_std']:.2f}"
        )
    best_gamma = max(frontier, key=lambda r: r["pnl_mean"])["gamma"]

    progress("tuning queue-imbalance weight on training sessions ...")
    imb_rows = []
    for c in cfg.imbalance_coefs:
        s = summarize(
            run_sessions(
                market, maker_config("glft+imbalance", calib, best_gamma, cfg.base_latency, c), cfg.train_seeds, W
            )
        )
        imb_rows.append({"coef": c, "pnl_mean": s["pnl_mean"], "sharpe_per_hour": s["sharpe_per_hour"]})
        progress(f"  coef={c:<5} pnl/h={s['pnl_mean']:8.1f}  sharpe/h={s['sharpe_per_hour']:6.2f}")
    best_imb = max(imb_rows, key=lambda r: r["pnl_mean"])["coef"]

    # --- 2. out-of-sample comparison ----------------------------------------
    progress(f"testing on {len(cfg.test_seeds)} unseen sessions (gamma={best_gamma}, imbalance={best_imb}) ...")
    kinds = ["symmetric", "glft", "glft+imbalance", "glft+lead"]
    raw_for_haircut = None
    per_kind = {}
    for kind in kinds:
        keep = kind == "glft"
        sessions = run_sessions(
            market, maker_config(kind, calib, best_gamma, cfg.base_latency, best_imb), cfg.test_seeds, W, keep_raw=keep
        )
        if keep:
            raw_for_haircut = [s.pop("_raw") for s in sessions]
        per_kind[kind] = sessions
    comparison = {k: summarize(v) for k, v in per_kind.items()}
    for k, s in comparison.items():
        progress(
            f"  {k:<15} pnl/h={s['pnl_mean']:8.1f} [{s['pnl_ci95'][0]:.0f}, {s['pnl_ci95'][1]:.0f}]  "
            f"sharpe/h={s['sharpe_per_hour']:6.2f}  informed share={s['informed_share']:.1%}"
        )
    paired = {
        "glft_vs_symmetric": paired_difference(per_kind["glft"], per_kind["symmetric"]),
        "imbalance_vs_glft": paired_difference(per_kind["glft+imbalance"], per_kind["glft"]),
        "lead_vs_glft": paired_difference(per_kind["glft+lead"], per_kind["glft"]),
        "glft_vs_symmetric_minute_vol": paired_difference(
            per_kind["glft"], per_kind["symmetric"], "pnl_vol_per_minute"
        ),
    }
    haircut = queue_haircut(raw_for_haircut, calib)
    progress(
        f"  queue haircut: model predicts {haircut['predicted_fills']:.0f} fills, maker got "
        f"{haircut['realized_fills']:.0f} ({haircut['ratio']:.0%})"
    )

    # --- 3. latency sweep -----------------------------------------------------
    progress(f"latency sweep on {len(cfg.latency_seeds)} more unseen sessions ...")
    sweep = []
    keys = ("pnl_mean", "pnl_ci95", "sharpe_per_hour", "informed_share", "lots_per_hour", "capture_per_lot")
    for ms in cfg.latencies_ms:
        lat = ms / 1000.0
        row = {"latency_ms": ms}
        runs = {}
        for kind in kinds:
            runs[kind] = run_sessions(
                market, maker_config(kind, calib, best_gamma, lat, best_imb), cfg.latency_seeds, W
            )
            s = summarize(runs[kind])
            row[kind] = {k: s[k] for k in keys}
            row[kind]["orders_per_hour"] = (
                float(np.mean([x["maker_orders"] for x in runs[kind]])) * 3600.0 / cfg.duration
            )
        row["signal_value"] = paired_difference(runs["glft+lead"], runs["glft"])
        sweep.append(row)
        progress(f"  {ms:5d} ms  " + "  ".join(f"{k}={row[k]['pnl_mean']:8.1f}" for k in kinds))
    breakeven = {kind: breakeven_latency(sweep, kind) for kind in kinds}
    progress(
        "  latency at which P&L reaches zero: "
        + ", ".join(
            f"{k}: {'never' if v is None else ('<0' if v < 0 else f'{v:.0f} ms')}" for k, v in breakeven.items()
        )
    )

    return {
        "config": {k: (list(v) if isinstance(v, (tuple, range)) else v) for k, v in asdict(cfg).items()},
        "market": {k: getattr(market, k) for k in dir(market) if not k.startswith("_")},
        "calibration": calib,
        "tuning": {
            "gamma_frontier": frontier,
            "best_gamma": best_gamma,
            "imbalance": imb_rows,
            "best_imbalance": best_imb,
        },
        "comparison": comparison,
        "paired": paired,
        "queue_haircut": haircut,
        "latency_sweep": sweep,
        "breakeven_latency_ms": breakeven,
        "runtime_seconds": round(time.time() - t0, 1),
    }


def breakeven_latency(sweep: list[dict], kind: str) -> float | None:
    """Latency (ms) at which a strategy's mean P&L first falls to zero (linear interpolation).

    Returns -1.0 if it already loses money at the lowest latency tested, and
    None if it never reaches zero within the sweep.
    """
    xs = [row["latency_ms"] for row in sweep]
    ys = [row[kind]["pnl_mean"] for row in sweep]
    if ys[0] <= 0:
        return -1.0
    for i in range(1, len(xs)):
        if ys[i - 1] > 0 >= ys[i]:
            return float(xs[i - 1] + (xs[i] - xs[i - 1]) * ys[i - 1] / (ys[i - 1] - ys[i]))
    return None
