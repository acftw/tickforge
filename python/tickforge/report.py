"""Charts for the README, drawn from the saved JSON results."""

from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# Validated categorical palette (fixed order) and neutral inks.
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
INK, INK2, MUTED, GRID, SURFACE = "#0b0b0b", "#52514e", "#8a8984", "#e6e5e1", "#fcfcfb"
NAMES = {
    "symmetric": "Symmetric (join the touch)",
    "glft": "GLFT (inventory-aware)",
    "glft+imbalance": "GLFT + queue imbalance",
    "glft+lead": "GLFT + lead signal",
}
KINDS = list(NAMES)


def _style():
    plt.rcParams.update(
        {
            "figure.facecolor": SURFACE,
            "axes.facecolor": SURFACE,
            "savefig.facecolor": SURFACE,
            "axes.edgecolor": GRID,
            "axes.labelcolor": INK2,
            "axes.titlecolor": INK,
            "axes.titlesize": 12,
            "axes.titleweight": "bold",
            "axes.titlelocation": "left",
            "axes.labelsize": 10,
            "xtick.color": INK2,
            "ytick.color": INK2,
            "xtick.labelsize": 9,
            "ytick.labelsize": 9,
            "axes.grid": True,
            "grid.color": GRID,
            "grid.linewidth": 0.8,
            "axes.spines.top": False,
            "axes.spines.right": False,
            "legend.frameon": False,
            "legend.fontsize": 9,
            "font.size": 10,
            "lines.linewidth": 2.0,
            "lines.markersize": 6,
        }
    )


def _save(fig, path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=160, bbox_inches="tight")
    plt.close(fig)
    return path


def fig_replication(rep: dict, path: Path) -> Path:
    rows = rep["rows"]
    fig, ax = plt.subplots(figsize=(8, 3.9))
    labels = [f"γ={r['gamma']}\n{r['strategy']}" for r in rows]
    y = np.arange(len(rows))
    ours = np.array([r["profit"] for r in rows])
    se = np.array([r["profit_se"] for r in rows])
    paper = np.array([r["paper_profit"] for r in rows])
    ax.scatter(
        paper,
        y,
        marker="D",
        s=120,
        facecolor="none",
        edgecolor=SERIES[1],
        linewidth=2,
        label="Published (Avellaneda & Stoikov 2008)",
        zorder=3,
    )
    ax.errorbar(
        ours,
        y,
        xerr=1.96 * se,
        fmt="o",
        color=SERIES[0],
        ecolor=SERIES[0],
        elinewidth=2,
        capsize=0,
        markersize=5,
        label="tickforge (95% CI, 1,000 runs)",
        zorder=4,
    )
    ax.set_yticks(y, labels)
    ax.invert_yaxis()
    ax.set_xlabel("Mean profit per simulation")
    ax.set_title("Replication of Avellaneda & Stoikov (2008), Tables 1–3")
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.2), ncol=2)
    ax.grid(axis="y", visible=False)
    return _save(fig, path)


def fig_strategies(study: dict, path: Path) -> Path:
    comp = study["comparison"]
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.4), gridspec_kw={"wspace": 0.55})
    y = np.arange(len(KINDS))
    names = [NAMES[k] for k in KINDS]
    ax = axes[0]
    for i, k in enumerate(KINDS):
        s = comp[k]
        lo, hi = s["pnl_ci95"]
        ax.plot([lo, hi], [i, i], color=SERIES[i], linewidth=3, solid_capstyle="round")
        ax.plot(s["pnl_mean"], i, "o", color=SERIES[i], markersize=8, markeredgecolor=SURFACE, markeredgewidth=1.5)
        ax.annotate(
            f"{s['pnl_mean']:+,.0f}",
            (max(hi, s["pnl_mean"]), i),
            xytext=(8, 0),
            textcoords="offset points",
            va="center",
            color=INK,
            fontsize=9,
        )
    ax.axvline(0, color=INK2, linewidth=1)
    ax.set_yticks(y, names)
    ax.invert_yaxis()
    ax.grid(axis="y", visible=False)
    ax.set_xlabel("P&L per hour, ticks (95% CI)")
    ax.set_title("Out-of-sample P&L")
    ax.margins(x=0.25)

    ax = axes[1]
    shares = [comp[k]["informed_share"] * 100 for k in KINDS]
    ax.barh(y, shares, color=SERIES[: len(KINDS)], height=0.6)
    for i, v in enumerate(shares):
        ax.annotate(f"{v:.0f}%", (v, i), xytext=(6, 0), textcoords="offset points", va="center", color=INK, fontsize=9)
    ax.set_yticks(y, [""] * len(KINDS))
    ax.invert_yaxis()
    ax.grid(axis="y", visible=False)
    ax.set_xlabel("% of filled lots traded against informed traders")
    ax.set_title("Adverse selection")
    ax.margins(x=0.2)
    lat = study["config"]["base_latency"] * 1000
    fig.suptitle(
        f"{study['comparison']['glft']['sessions']} unseen one-hour sessions, {lat:.0f} ms latency",
        x=0.125,
        y=1.04,
        ha="left",
        color=INK2,
        fontsize=10,
    )
    return _save(fig, path)


def fig_markouts(study: dict, path: Path) -> Path:
    comp = study["comparison"]
    fig, ax = plt.subplots(figsize=(7.5, 3.8))
    for i, k in enumerate(KINDS):
        mk = comp[k]["markouts"]
        hs = np.array([float(h) for h in mk])
        vals = np.array([mk[h] for h in mk])
        h_all = np.concatenate([[0.0], hs])
        v_all = np.concatenate([[comp[k]["capture_per_lot"]], vals])
        ax.plot(np.maximum(h_all, 0.03), v_all, "-o", color=SERIES[i], markersize=5, label=NAMES[k])
        ax.annotate(
            f"{v_all[-1]:+.2f}",
            (h_all[-1], v_all[-1]),
            xytext=(6, 0),
            textcoords="offset points",
            va="center",
            color=INK,
            fontsize=9,
        )
    ax.axhline(0, color=INK2, linewidth=1)
    ax.set_xscale("log")
    ticks = [0.03, 0.1, 0.5, 1, 5, 10, 30, 60]
    ax.set_xticks(ticks, ["at fill", "0.1s", "0.5s", "1s", "5s", "10s", "30s", "60s"])
    ax.minorticks_off()
    ax.set_ylabel("Ticks earned per lot")
    ax.set_xlabel("Measured at the mid this long after the fill")
    ax.set_title("Markouts: how much of the spread each strategy keeps")
    ax.legend(loc="lower left", fontsize=8.5)
    return _save(fig, path)


def fig_latency(study: dict, path: Path) -> Path:
    sweep = study["latency_sweep"]
    x = np.array([r["latency_ms"] for r in sweep], dtype=float)
    fig, axes = plt.subplots(
        2, 1, figsize=(8, 6.4), sharex=True, gridspec_kw={"height_ratios": [2.2, 1], "hspace": 0.18}
    )
    ax = axes[0]
    for i, k in enumerate(KINDS):
        m = np.array([r[k]["pnl_mean"] for r in sweep])
        lo = np.array([r[k]["pnl_ci95"][0] for r in sweep])
        hi = np.array([r[k]["pnl_ci95"][1] for r in sweep])
        ax.fill_between(x, lo, hi, color=SERIES[i], alpha=0.15, linewidth=0)
        ax.plot(x, m, "-o", color=SERIES[i], markersize=5, label=NAMES[k])
    ax.axhline(0, color=INK2, linewidth=1)
    be = study["breakeven_latency_ms"]
    for i, k in enumerate(KINDS):
        v = be.get(k)
        if v is not None and v > 0:
            ax.plot([v], [0], marker="|", markersize=16, color=SERIES[i], markeredgewidth=2.5)
    ax.set_xscale("symlog", linthresh=1)
    ax.set_ylabel("P&L per hour, ticks (95% CI)")
    ax.set_title("How fast does a market maker need to be?")
    ax.legend(loc="lower left", fontsize=8.5)

    ax = axes[1]
    for i, k in enumerate(KINDS):
        ax.plot(x, [r[k]["informed_share"] * 100 for r in sweep], "-o", color=SERIES[i], markersize=4)
    ax.set_ylabel("% lots vs informed")
    ax.set_xlabel("Maker latency (ms, decision to order reaching the book)")
    ax.set_xticks(
        [0, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000],
        ["0", "1", "2", "5", "10", "20", "50", "100", "200", "500", "1000"],
    )
    ax.minorticks_off()
    return _save(fig, path)


def fig_frontier(study: dict, path: Path) -> Path:
    rows = study["tuning"]["gamma_frontier"]
    best = study["tuning"]["best_gamma"]
    g = np.array([r["gamma"] for r in rows])
    fig, axes = plt.subplots(2, 1, figsize=(7.5, 5), sharex=True, gridspec_kw={"hspace": 0.15})
    for ax, key, label in (
        (axes[0], "pnl_mean", "Mean P&L per hour"),
        (axes[1], "pnl_vol_per_minute", "P&L volatility per minute"),
    ):
        y = np.array([r[key] for r in rows])
        ax.plot(g, y, "-o", color=SERIES[0], markersize=6)
        b = y[g == best][0]
        ax.plot(
            [best], [b], "o", color=SERIES[1], markersize=10, markeredgecolor=SURFACE, markeredgewidth=1.5, zorder=3
        )
        ax.annotate(
            f"γ = {best:g} (chosen)", (best, b), xytext=(10, 6), textcoords="offset points", fontsize=9, color=INK
        )
        ax.set_ylabel(label + ", ticks")
        if key == "pnl_mean":
            ax.axhline(0, color=INK2, linewidth=1)
    axes[0].set_title("Tuning GLFT risk aversion on training sessions")
    axes[1].set_xscale("log")
    axes[1].set_xticks(g, [f"{x:g}" for x in g])
    axes[1].minorticks_off()
    axes[1].set_xlabel("Risk aversion γ (log scale)")
    return _save(fig, path)


def fig_benchmark(bench: dict, path: Path) -> Path:
    fig, axes = plt.subplots(1, 2, figsize=(9, 2.6), gridspec_kw={"wspace": 0.45})
    names = ["tickforge", "std::map book"]
    tp = [bench["fast"]["msgs_per_sec_millions"], bench["reference"]["msgs_per_sec_millions"]]
    p99 = [bench["fast"]["p99_ns"], bench["reference"]["p99_ns"]]
    for ax, vals, title, fmt in (
        (axes[0], tp, "Throughput (million messages/s)", "{:.1f}M"),
        (axes[1], p99, "99th-percentile latency (ns)", "{:,.0f} ns"),
    ):
        ax.barh([0, 1], vals, color=[SERIES[0], MUTED], height=0.55)
        for i, v in enumerate(vals):
            ax.annotate(
                fmt.format(v), (v, i), xytext=(6, 0), textcoords="offset points", va="center", fontsize=9, color=INK
            )
        ax.set_yticks([0, 1], names)
        ax.invert_yaxis()
        ax.grid(axis="y", visible=False)
        ax.set_title(title, fontsize=11)
        ax.margins(x=0.25)
    return _save(fig, path)


def make_figures(study: dict, replication: dict, benchmark: dict | None, img_dir: Path) -> list[Path]:
    _style()
    img_dir = Path(img_dir)
    out = [
        fig_replication(replication, img_dir / "as_replication.png"),
        fig_strategies(study, img_dir / "strategies.png"),
        fig_markouts(study, img_dir / "markouts.png"),
        fig_latency(study, img_dir / "latency.png"),
        fig_frontier(study, img_dir / "gamma_frontier.png"),
    ]
    if benchmark:
        out.append(fig_benchmark(benchmark, img_dir / "benchmark.png"))
    return out
