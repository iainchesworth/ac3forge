"""Append one CI run's gold-reference SNR results to the quality-history
branch's per-branch JSONL file, and warn (non-blocking) on a trailing-baseline
regression.

Reads one JSON file per (leg, codec) - compare_wav.py's --json-out schema,
collected under --results-dir/<leg>/<codec>.json by CI's download-artifact
step - and appends one JSONL record per (leg, codec) to
<history-dir>/<branch>.jsonl.

This only ever runs after scripts/verify-gold-reference.sh has already passed
on every leg (see .github/workflows/_build.yml: the job that calls this
`needs: build`, which fails - and skips this - if any required leg's gate
failed), so what lands in history is never a broken run's numbers. The
regression check below is a separate, softer signal on top of that hard gate:
a relative-to-history warning, not a threshold this script can fail CI with.

stdlib-only (json/argparse/pathlib), matching compare_wav.py's own
no-new-CI-provisioning reasoning. Takes commit and commit-date as arguments
rather than reading the clock itself - see docs/quality-trend.md and this
project's general rule against Date.now()-style nondeterminism in tooling;
the caller sources commit-date from `git show -s --format=%cI`, i.e. the
commit's own recorded date, not whenever this script happens to run.
"""

import argparse
import json
import sys
from pathlib import Path

# How many trailing same-(branch,leg,codec) entries the regression check
# averages over, and how far below that average (in dB) counts as worth
# flagging. Both are judgment calls, not derived from anything - loose enough
# that ordinary cross-run noise (a compiler minor-version bump, a different
# runner's libm) shouldn't trip it on its own.
REGRESSION_TRAILING_WINDOW = 10
REGRESSION_DROP_DB = 3.0


def load_leg_results(results_dir: Path):
    """results_dir holds one subdirectory per leg (an artifact named
    'gold-reference-<preset>', downloaded with the prefix stripped back off by
    the caller - see the workflow step), each holding one JSON file per
    codec."""
    for leg_dir in sorted(results_dir.iterdir()):
        if not leg_dir.is_dir():
            continue
        leg = leg_dir.name.removeprefix("gold-reference-")
        for json_file in sorted(leg_dir.glob("*.json")):
            record = json.loads(json_file.read_text())
            record["leg"] = leg
            yield record


def trailing_mean(history_path: Path, leg: str, codec: str, window: int):
    if not history_path.exists():
        return None
    matches = []
    for line in history_path.read_text().splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        if rec.get("leg") == leg and rec.get("codec") == codec:
            matches.append(rec["worst_db"])
    if not matches:
        return None
    tail = matches[-window:]
    return sum(tail) / len(tail)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results-dir", type=Path, required=True)
    parser.add_argument("--history-dir", type=Path, required=True)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--commit-date", required=True,
                         help="Committer date, ISO 8601 (from `git show -s --format=%%cI`).")
    args = parser.parse_args()

    history_path = args.history_dir / f"{args.branch}.jsonl"
    records = list(load_leg_results(args.results_dir))
    if not records:
        print("::warning::no gold-reference JSON results found under "
              f"{args.results_dir}; nothing to append")
        return 0

    lines = []
    for rec in records:
        baseline = trailing_mean(history_path, rec["leg"], rec["codec"], REGRESSION_TRAILING_WINDOW)
        entry = {
            "commit": args.commit,
            "branch": args.branch,
            "commit_date": args.commit_date,
            "leg": rec["leg"],
            "codec": rec["codec"],
            "bitrate_kbps": rec["bitrate_kbps"],
            "worst_db": rec["worst_db"],
            "channels_db": rec["channels_db"],
            "threshold_db": rec["threshold_db"],
        }
        lines.append(json.dumps(entry, sort_keys=True))
        if baseline is not None and (baseline - rec["worst_db"]) >= REGRESSION_DROP_DB:
            print(f"::warning title=Quality trend regression::{rec['leg']}/{rec['codec']}: "
                  f"worst-channel SNR {rec['worst_db']:.2f} dB is "
                  f"{baseline - rec['worst_db']:.2f} dB below the trailing "
                  f"{REGRESSION_TRAILING_WINDOW}-run mean ({baseline:.2f} dB) on {args.branch}. "
                  f"Still above the hard {rec['threshold_db']:.0f} dB gate - this is a trend "
                  "warning, not a failure.")

    args.history_dir.mkdir(parents=True, exist_ok=True)
    with history_path.open("a") as f:
        for line in lines:
            f.write(line + "\n")

    print(f"Appended {len(lines)} record(s) to {history_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
