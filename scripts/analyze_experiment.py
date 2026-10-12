#!/usr/bin/env python3
"""Summarise an experiment CSV exported by traffic_cli or the server.

    python3 scripts/analyze_experiment.py results.csv [--plot delay.png]

Prints per-controller means with 95% confidence intervals and paired differences
against the first controller (the baseline). Uses only the standard library;
--plot additionally needs matplotlib.
"""
import argparse
import csv
import math
import statistics
from collections import defaultdict

T95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228,
       15: 2.131, 20: 2.086, 30: 2.042}


def t95(dof):
    if dof <= 0:
        return 0.0
    keys = [k for k in sorted(T95) if k <= dof]
    return T95[keys[-1]] if dof <= 30 else 1.96


def ci(xs):
    if len(xs) < 2:
        return 0.0
    return t95(len(xs) - 1) * statistics.stdev(xs) / math.sqrt(len(xs))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--metric", default="avg_delay_all_s")
    ap.add_argument("--plot", help="write a bar chart of the metric to this PNG")
    args = ap.parse_args()

    rows = list(csv.DictReader(open(args.csv, newline="")))
    if not rows:
        raise SystemExit("no rows in " + args.csv)
    controllers = list(dict.fromkeys(r["controller"] for r in rows))
    by = defaultdict(dict)  # controller -> seed -> row
    for r in rows:
        by[r["controller"]][r["seed"]] = r
    metrics = [k for k in rows[0] if k.endswith(("_s", "_veh", "_veh_per_h", "_stops", "_count", "_fraction"))]

    print(f"scenario {rows[0]['scenario_id']}, {len(by[controllers[0]])} seeds, duration {rows[0]['duration_s']} s\n")
    print(f"{'metric':34}" + "".join(f"{c:>24}" for c in controllers))
    for m in metrics:
        cells = []
        for c in controllers:
            xs = [float(r[m]) for r in by[c].values() if r[m] != ""]
            cells.append(f"{statistics.mean(xs):12.2f} ± {ci(xs):<8.2f}" if xs else f"{'n/a':>22}")
        print(f"{m:34}" + "".join(f"{x:>24}" for x in cells))

    base = controllers[0]
    print(f"\npaired differences in {args.metric} versus {base}:")
    for c in controllers[1:]:
        seeds = sorted(set(by[base]) & set(by[c]))
        diffs = [float(by[c][s][args.metric]) - float(by[base][s][args.metric]) for s in seeds]
        mean = statistics.mean(diffs)
        half = ci(diffs)
        verdict = "distinguishable from noise" if len(diffs) > 1 and abs(mean) > half else "within seed-to-seed noise"
        print(f"  {c:>14}: {mean:+.2f} (95% CI {mean - half:+.2f} .. {mean + half:+.2f}) — {verdict}")

    if args.plot:
        import matplotlib.pyplot as plt  # optional dependency
        means = [statistics.mean(float(r[args.metric]) for r in by[c].values()) for c in controllers]
        errs = [ci([float(r[args.metric]) for r in by[c].values()]) for c in controllers]
        plt.bar(controllers, means, yerr=errs, capsize=6, color=["#5C7389", "#1F6B4F", "#B7791F"][: len(controllers)])
        plt.ylabel(args.metric)
        plt.title(rows[0]["scenario_id"])
        plt.tight_layout()
        plt.savefig(args.plot, dpi=150)
        print("wrote", args.plot)


if __name__ == "__main__":
    main()
