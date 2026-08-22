"""Append one CI run's heap-churn results to the quality-history branch's
per-branch memory JSONL file, and flag regressions at the same two tiers
append_performance_history.py uses: a soft one (::warning::, never fails
this script) and a hard one (::error::, signalled via $GITHUB_OUTPUT's
hard_regression so the caller can fail the job *after* still committing and
pushing the data - the same never-silently-un-recorded reasoning).

This is the memory half of the trend machinery (docs/performance-trend.md's
"Memory trend" section): tests/performance/bench_memory.cpp's ac3membench
counts every heap allocation the codec makes per frame, per workload, and
this script keeps the series. Mirrors append_performance_history.py by
design (same JSONL-on-quality-history mechanics, same trailing window, same
percentage tiers) with two memory-specific differences:

- TWO churn metrics per record are checked against their own trailing
  means, not one: allocs_per_frame (allocator round-trips) and
  bytes_per_frame (allocator traffic). Either one regressing flags the
  record. Unlike ms/frame these are near-deterministic for a fixed
  workload, so a flag here is a real change in the code's allocation
  behaviour, not runner noise - and a trailing mean of zero (a fully
  buffer-reusing path, where the plan is headed) is handled by an absolute
  floor rather than a ratio, since any climb off zero is worth a look.

- steady_live_growth gets an ABSOLUTE check, not a trend one: live bytes
  still held after 199 steady-state frames is a leak signal, and a leak is
  a leak regardless of what last week's runs did. Warn above 4 KiB, hard
  above 1 MiB.

stdlib-only, commit/commit-date taken as arguments - same reasoning as
every other append script here (see append_performance_history.py).
"""

import argparse
import json
import os
import sys
from pathlib import Path

REGRESSION_TRAILING_WINDOW = 10
REGRESSION_GROWTH_FRACTION = 0.20
HARD_REGRESSION_GROWTH_FRACTION = 1.0
# Ratio checks need a non-trivial baseline; below these, a climb is judged
# by the absolute floor instead (a series sitting at/near zero should stay
# there).
ALLOCS_ABSOLUTE_FLOOR = 5.0
BYTES_ABSOLUTE_FLOOR = 4096.0
# steady_live_growth absolute tiers (bytes retained across the ~199
# steady-state frames): anything past noise is a leak worth a warning;
# a mebibyte is a leak worth failing over.
LIVE_GROWTH_WARN_BYTES = 4096
LIVE_GROWTH_HARD_BYTES = 1024 * 1024

CHURN_METRICS = (
    ("allocs_per_frame", "allocs/frame", ALLOCS_ABSOLUTE_FLOOR),
    ("bytes_per_frame", "bytes/frame", BYTES_ABSOLUTE_FLOOR),
)


def load_leg_results(results_dir: Path):
    """results_dir holds one subdirectory per leg (memory-<preset>), each
    holding bench_memory.cpp's --json-out file - the same layout contract as
    append_performance_history.py's, over ac3membench's schema."""
    for leg_dir in sorted(results_dir.iterdir()):
        if not leg_dir.is_dir():
            continue
        leg = leg_dir.name.removeprefix("memory-")
        for json_file in sorted(leg_dir.glob("*.json")):
            payload = json.loads(json_file.read_text())
            for result in payload["results"]:
                yield {
                    "leg": leg,
                    "config": result["name"],
                    "frames": result["frames"],
                    "setup_allocs": result["setup_allocs"],
                    "setup_bytes": result["setup_bytes"],
                    "first_allocs": result["first_allocs"],
                    "first_bytes": result["first_bytes"],
                    "allocs_per_frame": result["allocs_per_frame"],
                    "bytes_per_frame": result["bytes_per_frame"],
                    "steady_live_growth": result["steady_live_growth"],
                    "peak_live_delta": result["peak_live_delta"],
                    "peak_rss_bytes": payload["peak_rss_bytes"],
                }


def trailing_mean(history_path: Path, leg: str, config: str, metric: str, window: int):
    if not history_path.exists():
        return None
    matches = []
    for line in history_path.read_text().splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        if rec.get("leg") == leg and rec.get("config") == config and metric in rec:
            matches.append(rec[metric])
    if not matches:
        return None
    tail = matches[-window:]
    return sum(tail) / len(tail)


def emit_github_output(name: str, value: str) -> None:
    """No-op outside GitHub Actions (GITHUB_OUTPUT unset)."""
    path = os.environ.get("GITHUB_OUTPUT")
    if not path:
        return
    with open(path, "a") as f:
        f.write(f"{name}={value}\n")


def check_churn(rec, history_path: Path):
    """Yield (hard, message) for each churn metric that regressed against
    its own trailing mean - or, for a near-zero baseline, its absolute
    floor."""
    for metric, label, floor in CHURN_METRICS:
        value = rec[metric]
        baseline = trailing_mean(history_path, rec["leg"], rec["config"], metric,
                                 REGRESSION_TRAILING_WINDOW)
        if baseline is None:
            continue
        if baseline <= floor:
            if value > floor and value > baseline:
                yield (False,
                       f"{rec['leg']}/{rec['config']}: {label} climbed to {value:.1f} "
                       f"from a near-zero trailing mean ({baseline:.1f}). Allocation "
                       "counts are deterministic - something started allocating that "
                       "didn't before.")
            continue
        growth = (value - baseline) / baseline
        if growth >= HARD_REGRESSION_GROWTH_FRACTION:
            yield (True,
                   f"{rec['leg']}/{rec['config']}: {value:.1f} {label} is "
                   f"{growth * 100:.0f}% above the trailing "
                   f"{REGRESSION_TRAILING_WINDOW}-run mean ({baseline:.1f}) - past the "
                   f"{HARD_REGRESSION_GROWTH_FRACTION * 100:.0f}% hard threshold. Still "
                   "recorded; the run is failed separately so this doesn't go unnoticed.")
        elif growth >= REGRESSION_GROWTH_FRACTION:
            yield (False,
                   f"{rec['leg']}/{rec['config']}: {value:.1f} {label} is "
                   f"{growth * 100:.0f}% above the trailing "
                   f"{REGRESSION_TRAILING_WINDOW}-run mean ({baseline:.1f}). A trend "
                   "warning, not a failure - but unlike ms/frame this number does not "
                   "wobble with runner load.")


def check_leak(rec):
    growth = rec["steady_live_growth"]
    if growth >= LIVE_GROWTH_HARD_BYTES:
        return (True,
                f"{rec['leg']}/{rec['config']}: {growth} bytes still live after the "
                f"steady-state frames - above the {LIVE_GROWTH_HARD_BYTES} hard leak "
                "threshold.")
    if growth >= LIVE_GROWTH_WARN_BYTES:
        return (False,
                f"{rec['leg']}/{rec['config']}: {growth} bytes still live after the "
                "steady-state frames - possible slow leak or unbounded cache growth.")
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results-dir", type=Path, required=True)
    parser.add_argument("--history-dir", type=Path, required=True)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--commit-date", required=True,
                        help="Committer date, ISO 8601 (from `git show -s --format=%%cI`).")
    args = parser.parse_args()

    history_path = args.history_dir / f"memory-{args.branch}.jsonl"
    records = list(load_leg_results(args.results_dir))
    if not records:
        print("::warning::no memory JSON results found under "
              f"{args.results_dir}; nothing to append")
        return 0

    lines = []
    hard_regression = False
    for rec in records:
        findings = list(check_churn(rec, history_path))
        leak = check_leak(rec)
        if leak is not None:
            findings.append(leak)

        entry = {"commit": args.commit, "branch": args.branch,
                 "commit_date": args.commit_date, **rec}
        lines.append(json.dumps(entry, sort_keys=True))

        for hard, message in findings:
            if hard:
                hard_regression = True
                print(f"::error title=Memory trend hard regression::{message}")
            else:
                print(f"::warning title=Memory trend regression::{message}")

    args.history_dir.mkdir(parents=True, exist_ok=True)
    with history_path.open("a") as f:
        for line in lines:
            f.write(line + "\n")

    print(f"Appended {len(lines)} record(s) to {history_path}")
    emit_github_output("hard_regression", "true" if hard_regression else "false")
    return 0


if __name__ == "__main__":
    sys.exit(main())
