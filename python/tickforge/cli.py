"""Command-line interface.

tickforge replicate-as      rerun Avellaneda & Stoikov (2008) Tables 1-3
tickforge study             calibrate, tune, test out of sample, latency sweep
tickforge figures           draw the README charts from saved results
tickforge all               everything above (add --quick for a fast smoke run)
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np


def _dump(obj, path: Path) -> None:
    def default(o):
        if isinstance(o, np.generic):
            return o.item()
        if isinstance(o, np.ndarray):
            return o.tolist()
        raise TypeError(type(o))

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2, default=default))


def cmd_replicate(args) -> dict:
    from tickforge.as_model import replicate

    rows = replicate(n_paths=args.paths, seed=args.seed)
    print(f"Avellaneda & Stoikov (2008) replication, {args.paths} simulations per row\n")
    print(
        f"{'gamma':>6} {'strategy':<10} {'profit':>8} {'paper':>7} {'z':>6} {'std':>7} {'paper':>7} "
        f"{'std q':>7} {'paper':>7}"
    )
    for r in rows:
        z = (r["profit"] - r["paper_profit"]) / r["profit_se"]
        r["z_profit"] = round(z, 2)
        print(
            f"{r['gamma']:>6} {r['strategy']:<10} {r['profit']:>8.2f} {r['paper_profit']:>7.1f} {z:>6.2f} "
            f"{r['std_profit']:>7.2f} {r['paper_std_profit']:>7.1f} "
            f"{r['std_final_q']:>7.2f} {r['paper_std_final_q']:>7.1f}"
        )
    out = {"paths": args.paths, "seed": args.seed, "rows": rows}
    _dump(out, Path(args.out) / "as_replication.json")
    return out


def cmd_study(args) -> dict:
    from tickforge.study import StudyConfig, run_study

    cfg = StudyConfig.quick() if args.quick else StudyConfig()
    if args.workers:
        cfg.workers = args.workers
    res = run_study(cfg)
    _dump(res, Path(args.out) / ("study_quick.json" if args.quick else "study.json"))
    print(f"\nfinished in {res['runtime_seconds']} s")
    return res


def cmd_figures(args) -> None:
    from tickforge.report import make_figures

    out = Path(args.out)
    study_file = out / ("study_quick.json" if args.quick else "study.json")
    paths = make_figures(
        study=json.loads(study_file.read_text()),
        replication=json.loads((out / "as_replication.json").read_text()),
        benchmark=json.loads((out / "benchmark.json").read_text()) if (out / "benchmark.json").exists() else None,
        img_dir=Path(args.img) if args.img else (out / "img_quick" if args.quick else Path("docs/img")),
    )
    for p in paths:
        print("wrote", p)


def cmd_site(args) -> None:
    from tickforge.site import build

    for p in build(Path(args.out), Path(args.site)):
        print("wrote", p, f"({p.stat().st_size / 1024:.0f} KB)")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        prog="tickforge", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--out", default="results", help="directory for JSON results (default: results)")
        sp.add_argument("--quick", action="store_true", help="small, fast version for smoke tests")

    r = sub.add_parser("replicate-as", help="rerun Avellaneda & Stoikov (2008) Tables 1-3")
    common(r)
    r.add_argument("--paths", type=int, default=1000)
    r.add_argument("--seed", type=int, default=2008)

    s = sub.add_parser("study", help="run the market-making study")
    common(s)
    s.add_argument("--workers", type=int, default=0)

    f = sub.add_parser("figures", help="draw charts from saved results")
    common(f)
    f.add_argument("--img", default=None, help="chart folder (default: docs/img, or results/img_quick with --quick)")

    a = sub.add_parser("all", help="replication + study + figures")
    common(a)
    a.add_argument("--paths", type=int, default=1000)
    a.add_argument("--seed", type=int, default=2008)
    a.add_argument("--workers", type=int, default=0)
    a.add_argument("--img", default=None, help="chart folder (default: docs/img, or results/img_quick with --quick)")

    w = sub.add_parser("site", help="write the website's data files from saved results")
    w.add_argument("--out", default="results", help="directory holding study.json etc. (default: results)")
    w.add_argument("--site", default="docs", help="website folder (default: docs)")

    args = p.parse_args(argv)
    if args.cmd == "replicate-as":
        cmd_replicate(args)
    elif args.cmd == "study":
        cmd_study(args)
    elif args.cmd == "figures":
        cmd_figures(args)
    elif args.cmd == "site":
        cmd_site(args)
    else:
        cmd_replicate(args)
        cmd_study(args)
        cmd_figures(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
