"""Append one CI run's encoder throughput results to the quality-history
branch's per-branch performance JSONL file, and flag a trailing-baseline
slowdown at two tiers: a soft one (::warning::, never fails this script) and
a hard one (::error::, signalled via $GITHUB_OUTPUT's hard_regression so the
caller can fail the job *after* still committing and pushing the data - see
.github/workflows/ci.yml's "persist-performance-trend" job, which fails
after the push so a big regression is never silently un-recorded just
because it also failed the run).

This is the trend-tracking HALF of the performance suite, not the hard
real-time gate - that is tests/performance/test_performance.cpp's ac3perf
target, a separate CI-blocking ctest run on every push/PR. This script only
ever runs on develop/main pushes (mirrors persist_quality_trend's own
gating), records numbers ac3perf's pass/fail already implicitly bounds, and
raises a softer, trend-relative signal on top: not "did this exceed the
absolute real-time budget" (ac3perf's job) but "is this drifting slower over
time even while still passing that gate".

Reads one JSON file per leg - tests/performance/bench_encoder.cpp's
--json-out schema ({"real_time_budget_ms_per_frame": ..., "results": [{name,
frames, total_ms, ms_per_frame}, ...]}) - collected under
--results-dir/<leg>/*.json by CI's download-artifact step, and appends one
JSONL record per (leg, config) to <history-dir>/performance-<branch>.jsonl.
Reuses the quality-history branch (a different file, not a new branch) -
see docs/performance-trend.md.

Mirrors tools/ci/append_quality_history.py closely by design (same shape,
same reasoning) - the one real difference is direction: for quality, LOWER
dB is worse; for performance, HIGHER ms/frame is worse. Percentage-based
thresholds are used here rather than quality-trend's fixed dB deltas,
because ms/frame's absolute scale varies a lot across CI runner hardware in
a way dB (already a log-relative unit) does not.

stdlib-only (json/argparse/pathlib), matching compare_wav.py's/
append_quality_history.py's own no-new-CI-provisioning reasoning. Takes
commit and commit-date as arguments rather than reading the clock itself -
see this project's general rule against Date.now()-style nondeterminism in
tooling; the caller sources commit-date from `git show -s --format=%cI`.
"""

import argparse
import json
import os
import sys
from pathlib import Path

# How many trailing same-(branch,leg,config) entries the regression check
# averages over - same window append_quality_history.py uses, for the same
# reason (enough to smooth ordinary run-to-run noise without going stale).
REGRESSION_TRAILING_WINDOW = 10
# A run more than 20% slower than its own trailing mean is worth a table
# annotation, even though it may still comfortably pass ac3perf's absolute
# real-time gate.
REGRESSION_SLOWDOWN_FRACTION = 0.20
# A run at least DOUBLE its trailing mean is worth failing the job over
# outright, not just annotating - this is the threshold that would have
# caught the ~8-28x regression this whole suite exists because of, with
# enormous margin to spare.
HARD_REGRESSION_SLOWDOWN_FRACTION = 1.0


def load_leg_results(results_dir: Path):
    """results_dir holds one subdirectory per leg (an artifact named
    'performance-<preset>', downloaded with the prefix stripped back off by
    the caller - see the workflow step), each holding bench_encoder.cpp's
    --json-out file."""
    for leg_dir in sorted(results_dir.iterdir()):
        if not leg_dir.is_dir():
            continue
        leg = leg_dir.name.removeprefix("performance-")
        for json_file in sorted(leg_dir.glob("*.json")):
            payload = json.loads(json_file.read_text())
            for result in payload["results"]:
                yield {
                    "leg": leg,
                    "config": result["name"],
                    "frames": result["frames"],
                    "total_ms": result["total_ms"],
                    "ms_per_frame": result["ms_per_frame"],
                    "real_time_budget_ms_per_frame": payload["real_time_budget_ms_per_frame"],
                }


def trailing_mean(history_path: Path, leg: str, config: str, window: int):
    if not history_path.exists():
        return None
    matches = []
    for line in history_path.read_text().splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        if rec.get("leg") == leg and rec.get("config") == config:
            matches.append(rec["ms_per_frame"])
    if not matches:
        return None
    tail = matches[-window:]
    return sum(tail) / len(tail)


def emit_github_output(name: str, value: str) -> None:
    """No-op outside GitHub Actions (GITHUB_OUTPUT unset) - safe to call from
    a plain local run of this script."""
    path = os.environ.get("GITHUB_OUTPUT")
    if not path:
        return
    with open(path, "a") as f:
        f.write(f"{name}={value}\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results-dir", type=Path, required=True)
    parser.add_argument("--history-dir", type=Path, required=True)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--commit-date", required=True,
                         help="Committer date, ISO 8601 (from `git show -s --format=%%cI`).")
    args = parser.parse_args()

    history_path = args.history_dir / f"performance-{args.branch}.jsonl"
    records = list(load_leg_results(args.results_dir))
    if not records:
        print("::warning::no performance JSON results found under "
              f"{args.results_dir}; nothing to append")
        return 0

    lines = []
    hard_regression = False
    for rec in records:
        baseline = trailing_mean(history_path, rec["leg"], rec["config"],
                                 REGRESSION_TRAILING_WINDOW)
        entry = {
            "commit": args.commit,
            "branch": args.branch,
            "commit_date": args.commit_date,
            "leg": rec["leg"],
            "config": rec["config"],
            "frames": rec["frames"],
            "total_ms": rec["total_ms"],
            "ms_per_frame": rec["ms_per_frame"],
            "real_time_budget_ms_per_frame": rec["real_time_budget_ms_per_frame"],
        }
        lines.append(json.dumps(entry, sort_keys=True))

        slowdown = None if baseline is None or baseline <= 0 else (
            (rec["ms_per_frame"] - baseline) / baseline)
        if slowdown is not None and slowdown >= HARD_REGRESSION_SLOWDOWN_FRACTION:
            hard_regression = True
            print(f"::error title=Performance trend hard regression::{rec['leg']}/"
                  f"{rec['config']}: {rec['ms_per_frame']:.3f} ms/frame is "
                  f"{slowdown * 100:.0f}% slower than the trailing "
                  f"{REGRESSION_TRAILING_WINDOW}-run mean ({baseline:.3f} ms/frame) on "
                  f"{args.branch} - past the {HARD_REGRESSION_SLOWDOWN_FRACTION * 100:.0f}% "
                  "hard-regression threshold. Still recorded below; the run this came from is "
                  "failed separately so this doesn't go unnoticed.")
        elif slowdown is not None and slowdown >= REGRESSION_SLOWDOWN_FRACTION:
            print(f"::warning title=Performance trend regression::{rec['leg']}/"
                  f"{rec['config']}: {rec['ms_per_frame']:.3f} ms/frame is "
                  f"{slowdown * 100:.0f}% slower than the trailing "
                  f"{REGRESSION_TRAILING_WINDOW}-run mean ({baseline:.3f} ms/frame) on "
                  f"{args.branch}. This is a trend warning, not a failure - see ac3perf for "
                  "the absolute real-time gate.")

    args.history_dir.mkdir(parents=True, exist_ok=True)
    with history_path.open("a") as f:
        for line in lines:
            f.write(line + "\n")

    print(f"Appended {len(lines)} record(s) to {history_path}")
    emit_github_output("hard_regression", "true" if hard_regression else "false")
    return 0


if __name__ == "__main__":
    sys.exit(main())
