"""Append one CI run's external-encoder landscape scores to the
quality-history branch's per-branch JSONL file, and flag a trailing-baseline
regression at two tiers: a soft one (::warning::, never fails this script)
and a hard one (::error::, signalled via $GITHUB_OUTPUT's hard_regression so
the caller can fail the job *after* still committing and pushing the data -
same reasoning as append-quality-history.py/append-performance-history.py's
identical two-tier design, and the same "commit first, fail the job after"
ordering so a regression is never silently un-recorded just because it also
failed the run).

Reads tools/quality_race.py's `trend` mode JSON output (one row per
(leg, variant) for THIS build, scored entirely through ac3cli's own
decoder - see race_trend's docstring) plus the checked-in
tests/golden/external-baseline/manifest.json (the FFmpeg/DEE numbers from
tools/gen_external_baseline.py's last local run), and appends one JSONL
record per row to <history-dir>/external-comparison-<branch>.jsonl. Only
"landscape" rows (E-AC-3's "all"/AC-3's automatic tools - the number
actually comparable to FFmpeg's/DEE's own black-box output) get vs_ffmpeg/
vs_dee deltas; the per-tool detail rows (EAC3_VARIANTS/EAC3_SELF_VARIANTS)
have no matching external number to compare against, so those keys are
simply absent rather than null-padded. A leg's DEE score can itself be
{"status": "unverified", ...} in the manifest (see
tools/gen_external_baseline.py's UNVERIFIED_DEE_LEGS) - vs_dee_snr_db is
omitted for those rather than comparing against a number that was never
real.

Mirrors scripts/append-quality-history.py closely by design (same shape,
same trailing-10-run window, same regression thresholds - the underlying
metric is the same kind of dB-scale SNR number). This only runs on
develop/main pushes (mirrors persist_quality_trend's own gating) and only
after tools/quality_race.py's `ci` gate has already passed on the same
push, so what lands in history is never a broken run's numbers.

Also carries through each row's "mos_lqo" (ViSQOL MOS-LQO, see
quality_race.py's perceptual_score()) when the environment that produced
trend.json had `visqol-python` installed - null otherwise, same graceful-
degradation contract as everywhere else this project uses it, not a second
regression-gated metric: no trailing-window/soft/hard-regression handling
for it, mirroring lsd_db/hf_db's own read-only treatment here. vs_ffmpeg_
mos_lqo/vs_dee_mos_lqo are added on `landscape` rows the same way vs_*_snr_db
is, but only when *both* sides of the comparison have a real mos_lqo -
tools/gen_external_baseline.py's manifest predates this column for every
baseline generated before it, so most rows simply won't have the key yet.

stdlib-only (json/argparse/pathlib), matching every other append-*.py
script's no-new-CI-provisioning reasoning. Takes commit and commit-date as
arguments rather than reading the clock itself - see this project's general
rule against Date.now()-style nondeterminism in tooling; the caller sources
commit-date from `git show -s --format=%cI`.
"""

import argparse
import json
import os
import sys
from pathlib import Path

REGRESSION_TRAILING_WINDOW = 10
REGRESSION_DROP_DB = 0.5
HARD_REGRESSION_DROP_DB = 10.0


def load_trend_rows(trend_json: Path):
    payload = json.loads(trend_json.read_text())
    return payload["rows"]


def load_baseline_scores(manifest_json: Path):
    """leg -> {"ffmpeg": {...} | None, "dee": {...} | None, "baseline_version": int}
    - a tool's dict is None when the manifest marks it unverified (see
    tools/gen_external_baseline.py's UNVERIFIED_DEE_LEGS) rather than a
    real score, so callers never diff against a number that was never
    real."""
    manifest = json.loads(manifest_json.read_text())
    out = {}
    for leg, data in manifest["legs"].items():
        scores = {}
        for tool in ("ffmpeg", "dee"):
            entry = data["scores"].get(tool)
            scores[tool] = entry if entry and "snr_db" in entry else None
        out[leg] = {"scores": scores, "baseline_version": manifest["baseline_version"]}
    return out


def trailing_mean(history_path: Path, leg: str, variant: str, window: int):
    if not history_path.exists():
        return None
    matches = []
    for line in history_path.read_text().splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        if rec.get("leg") == leg and rec.get("variant") == variant:
            matches.append(rec["snr_db"])
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
    parser.add_argument("--trend-json", type=Path, required=True)
    parser.add_argument("--manifest-json", type=Path, required=True)
    parser.add_argument("--history-dir", type=Path, required=True)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--commit-date", required=True,
                         help="Committer date, ISO 8601 (from `git show -s --format=%%cI`).")
    args = parser.parse_args()

    history_path = args.history_dir / f"external-comparison-{args.branch}.jsonl"
    rows = load_trend_rows(args.trend_json)
    if not rows:
        print(f"::warning::no rows found in {args.trend_json}; nothing to append")
        return 0
    baselines = load_baseline_scores(args.manifest_json)

    lines = []
    hard_regression = False
    for row in rows:
        leg, variant = row["leg"], row["variant"]
        baseline = trailing_mean(history_path, leg, variant, REGRESSION_TRAILING_WINDOW)
        entry = {
            "commit": args.commit,
            "branch": args.branch,
            "commit_date": args.commit_date,
            "leg": leg,
            "codec": row["codec"],
            "bitrate_kbps": row["bitrate_kbps"],
            "variant": variant,
            "snr_db": row["snr_db"],
            "lsd_db": row["lsd_db"],
            "hf_db": row["hf_db"],
            "mos_lqo": row.get("mos_lqo"),
        }
        leg_baseline = baselines.get(leg)
        if leg_baseline is not None:
            entry["baseline_version"] = leg_baseline["baseline_version"]
            if variant == "landscape":
                for tool in ("ffmpeg", "dee"):
                    score = leg_baseline["scores"][tool]
                    if score is not None:
                        entry[f"vs_{tool}_snr_db"] = row["snr_db"] - score["snr_db"]
                        if entry["mos_lqo"] is not None and score.get("mos_lqo") is not None:
                            entry[f"vs_{tool}_mos_lqo"] = entry["mos_lqo"] - score["mos_lqo"]
        lines.append(json.dumps(entry, sort_keys=True))

        drop = None if baseline is None else baseline - row["snr_db"]
        if drop is not None and drop >= HARD_REGRESSION_DROP_DB:
            hard_regression = True
            print(f"::error title=External-comparison trend hard regression::{leg}/{variant}: "
                  f"SNR {row['snr_db']:.2f} dB is {drop:.2f} dB below the trailing "
                  f"{REGRESSION_TRAILING_WINDOW}-run mean ({baseline:.2f} dB) on {args.branch} - "
                  f"more than the {HARD_REGRESSION_DROP_DB:.0f} dB hard-regression threshold. "
                  "Still recorded below; the run this came from is failed separately so this "
                  "doesn't go unnoticed.")
        elif drop is not None and drop >= REGRESSION_DROP_DB:
            print(f"::warning title=External-comparison trend regression::{leg}/{variant}: "
                  f"SNR {row['snr_db']:.2f} dB is {drop:.2f} dB below the trailing "
                  f"{REGRESSION_TRAILING_WINDOW}-run mean ({baseline:.2f} dB) on {args.branch}. "
                  "This is a trend warning, not a failure - see `ci` mode for the absolute gate.")

    args.history_dir.mkdir(parents=True, exist_ok=True)
    with history_path.open("a") as f:
        for line in lines:
            f.write(line + "\n")

    print(f"Appended {len(lines)} record(s) to {history_path}")
    emit_github_output("hard_regression", "true" if hard_regression else "false")
    return 0


if __name__ == "__main__":
    sys.exit(main())
