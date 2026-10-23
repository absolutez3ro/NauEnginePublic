"""Validate MG-13 transfer pairs and summarize SSAO GPU timestamps, without deriving FPS."""

import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def summarize(path, warmup_frames):
    with path.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError(f"No GPU measurements in {path}")
    pairs = defaultdict(dict)
    groups = defaultdict(list)
    seen = set()
    for row in rows:
        frame, milliseconds = int(row["frame"]), float(row["ms"])
        if frame <= 0 or not math.isfinite(milliseconds) or milliseconds < 0:
            raise ValueError(f"Invalid timing in {path}: {row}")
        key = (frame, row["pass"], row["gpu"])
        if key in seen:
            raise ValueError(f"Duplicate measurement in {path}: {key}")
        seen.add(key)
        base, _, suffix = row["pass"].rpartition("/")
        if base.startswith("ssao_") and suffix in ("src", "dst", "sum", "span"):
            pairs[(frame, base)][suffix] = (milliseconds, row["gpu"])
        if frame > warmup_frames and (row["pass"] == "Frame" or row["pass"].startswith("ssao_")):
            groups[(row["pass"], row["gpu"])].append(milliseconds)
    paired_count = 0
    for (frame, name), halves in pairs.items():
        if "sum" not in halves:
            continue  # A dropped or unsupported half is not a zero-duration transfer.
        if "src" not in halves or "dst" not in halves:
            raise ValueError(f"Sum without both halves: {path}, {frame}, {name}")
        total = halves["src"][0] + halves["dst"][0]
        if abs(total - halves["sum"][0]) > 0.00021:  # Each CSV value is rounded to four decimals.
            raise ValueError(f"Incorrect transfer sum: {path}, {frame}, {name}")
        expected = halves["src"][1] + ">" + halves["dst"][1]
        if halves["sum"][1] != expected:
            raise ValueError(f"Incorrect transfer direction: {path}, {frame}, {name}")
        paired_count += 1
    if not groups:
        raise ValueError(f"No measurements after frame {warmup_frames}: {path}")
    prefix = path.with_name(path.name.removesuffix("-timings.csv"))
    metadata_path = prefix.with_suffix(".result.json")
    metadata = json.loads(metadata_path.read_text(encoding="utf-8-sig")) if metadata_path.exists() else {}
    return {
        "csv": str(path),
        "device": metadata.get("device", "unknown"),
        "result": metadata,
        "discarded_first_frames": warmup_frames,
        "validated_transfer_pairs": paired_count,
        "timings": [
            {"pass": name, "gpu": gpu, "samples": len(values), "mean_ms": statistics.mean(values),
             "p50_ms": statistics.median(values), "p95_ms": percentile(values, 0.95)}
            for (name, gpu), values in sorted(groups.items())
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path, nargs="+")
    parser.add_argument("--warmup-frames", type=int, default=10)
    parser.add_argument("--output", type=Path, required=True, help="Output prefix for JSON and Markdown")
    args = parser.parse_args()
    if args.warmup_frames < 0:
        parser.error("--warmup-frames must be nonnegative")
    reports = [summarize(path, args.warmup_frames) for path in args.csv]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.with_suffix(".json").write_text(json.dumps(reports, indent=2), encoding="utf-8")
    lines = ["# SSAO GPU timestamp measurements", "",
             "Durations come from GPU timestamps, not FPS. Frame on GPU 0 includes waiting for AO.",
             "COPY /sum is src + dst GPU work; /span uses each COPY queue's clock calibration.",
             "WARP runs validate the instrumentation and do not demonstrate multi-GPU speedup.", ""]
    for report in reports:
        lines += [f"## {Path(report['csv']).name}", "",
                  f"Device: `{report['device']}`. First {args.warmup_frames} frames excluded. "
                  f"Validated transfer pairs: {report['validated_transfer_pairs']}.", "",
                  "| Pass | GPU | Samples | Mean, ms | P50, ms | P95, ms |",
                  "| --- | --- | ---: | ---: | ---: | ---: |"]
        for timing in report["timings"]:
            lines.append(f"| {timing['pass']} | {timing['gpu']} | {timing['samples']} | "
                         f"{timing['mean_ms']:.4f} | {timing['p50_ms']:.4f} | {timing['p95_ms']:.4f} |")
        lines.append("")
    args.output.with_suffix(".md").write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps({"reports": len(reports), "output": str(args.output),
                      "validated_transfer_pairs": sum(item["validated_transfer_pairs"] for item in reports)}, indent=2))


if __name__ == "__main__":
    main()
