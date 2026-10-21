"""Compares GPU profiler CSVs (frame,pass,gpu,ms,mode) of 1gpu and 2gpu runs on one scene.

For every pass and GPU: median over frames after the warm-up, maximum separately, and the change of the median.
The first frames (shader compilation, resource creation) are dropped with --skip (default 30).

    py tools/scripts/compare_gpu_timings.py results/gpu_timings_1gpu.csv results/gpu_timings_2gpu.csv
    py tools/scripts/compare_gpu_timings.py ... --out results/zamery.csv --decimal-comma

Until the engine has a mode where both GPUs render, a 2gpu run is the same single-GPU work with another label, so
the 2gpu columns are marked "2gpu (без реального второго GPU)". Pass --real-2gpu once the second GPU really renders.
"""

import argparse
import csv
import statistics
import sys
from collections import defaultdict

FAKE_2GPU_LABEL = "2gpu (без реального второго GPU)"


def read_samples(paths, skip):
    """Returns {(mode, pass, gpu): [ms, ...]} and {mode: frames used} for frames after `skip`."""
    samples = defaultdict(list)
    frames = defaultdict(set)
    for path in paths:
        with open(path, newline="", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                try:
                    frame = int(row["frame"])
                    ms = float(row["ms"])
                except (KeyError, ValueError):
                    continue  # a truncated last line of a killed run
                if frame <= skip:
                    continue
                mode = row.get("mode") or "1gpu"
                samples[(mode, row["pass"], row["gpu"])].append(ms)
                frames[mode].add((path, frame))
    return samples, {mode: len(f) for mode, f in frames.items()}


def fmt(value, decimal_comma):
    if value is None:
        return ""
    text = f"{value:.3f}"
    return text.replace(".", ",") if decimal_comma else text


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv", nargs="+", help="profiler CSV files, any mix of 1gpu and 2gpu runs")
    parser.add_argument("--skip", type=int, default=30, help="warm-up frames to drop (default 30)")
    parser.add_argument("--out", help="also write the table as CSV (for the sheet)")
    parser.add_argument("--decimal-comma", action="store_true", help="decimal comma in --out (Russian locale sheets)")
    parser.add_argument("--real-2gpu", action="store_true", help="the 2gpu runs really render on two GPUs")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")  # the 2gpu label is Russian; pipes default to the ANSI code page

    samples, frames = read_samples(args.csv, args.skip)
    if not samples:
        sys.exit("no samples after the warm-up frames")

    label_2gpu = "2gpu" if args.real_2gpu else FAKE_2GPU_LABEL
    keys = sorted({(p, g) for (_, p, g) in samples}, key=lambda k: (k[1], k[0] != "Frame", k[0]))

    header = ["pass", "gpu", "1gpu median, ms", "1gpu max, ms", f"{label_2gpu} median, ms", f"{label_2gpu} max, ms",
              "median change, %"]
    rows = []
    for name, gpu in keys:
        stats = {}
        for mode in ("1gpu", "2gpu"):
            values = samples.get((mode, name, gpu))
            stats[mode] = (statistics.median(values), max(values)) if values else (None, None)
        m1, m2 = stats["1gpu"][0], stats["2gpu"][0]
        change = (m2 - m1) / m1 * 100.0 if m1 and m2 is not None else None
        rows.append([name, gpu, stats["1gpu"][0], stats["1gpu"][1], stats["2gpu"][0], stats["2gpu"][1], change])

    print(f"Frames after the first {args.skip}: " + ", ".join(f"{m} {n}" for m, n in sorted(frames.items())))
    if not args.real_2gpu:
        print(f"Note: {FAKE_2GPU_LABEL}: the engine has no mode where both GPUs render yet.")
    print()
    print("| " + " | ".join(header) + " |")
    print("|" + "---|" * len(header))
    for row in rows:
        print("| " + " | ".join([row[0], row[1]] + [fmt(v, False) for v in row[2:]]) + " |")

    if args.out:
        with open(args.out, "w", newline="", encoding="utf-8-sig") as f:
            writer = csv.writer(f, delimiter=";" if args.decimal_comma else ",")
            writer.writerow(header)
            for row in rows:
                writer.writerow([row[0], row[1]] + [fmt(v, args.decimal_comma) for v in row[2:]])
        print(f"\nwritten {args.out}")


if __name__ == "__main__":
    main()
