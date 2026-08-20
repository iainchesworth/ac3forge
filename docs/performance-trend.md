# Performance trend

Four separate mechanisms, not one, and it matters which is which:

- **The hard gate**: `ac3perf` (`tests/performance/test_performance.cpp`) asserts the
  encoder stays faster than real time (with a 2x safety margin), on every push and
  every PR. A failure here blocks CI outright - see
  [CI Status](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/workflows/ci.yml).
  Not run under the ASan/UBSan leg: instrumented code has nothing useful to say about
  throughput at any slack factor, so that leg excludes the `Performance` label entirely
  (`CMakePresets.json`'s `test-linux-llvm-asan-ubsan` preset).
- **This page's whole-frame tables**: `ac3bench` (`tests/performance/bench_encoder.cpp`)
  runs the same encoder configurations for longer (200 frames) and records the actual
  ms/frame number, not just a pass/fail, on every push to `develop`/`main`. It exists
  to answer a question the hard gate cannot: is throughput quietly drifting slower
  over time even while it keeps passing.
- **This page's per-kernel tables**: `ac3kernelbench`
  (`tests/performance/kernel_bench.cpp`) times each hot kernel in isolation (ns/call,
  fed real audio through the real windowing + forward MDCT) on the same pushes. It
  answers the question one level below `ac3bench`'s: when a whole-frame number
  drifts, *which stage* moved - without anyone having to reattach a profiler to find
  out.
- **This page's memory tables**: `ac3membench`
  (`tests/performance/bench_memory.cpp`) counts what the others time: heap
  allocations and allocator traffic per frame, live-byte drift, and peak RSS,
  across the same encoder configurations *plus* the decode paths the timing
  benches never covered. It records the memory-usage programme's progress the
  same way the whole-frame series recorded the CPU programme's, and - unlike
  ms/frame - its numbers are near-deterministic for a fixed workload, so a
  flagged row is a real behavioural change, not runner noise.

All of this exists because a severe encoder regression (a per-call recomputation the
forward MDCT should have cached) once shipped with no coverage to catch it: the hard
gate blocks a repeat outright, and the trend tables catch the gradual drift a
pass/fail gate cannot see.

`tools/ci/append_performance_history.py` appends every `develop`/`main` run's numbers
to the `quality-history` branch (reused, not a new branch - the same reasoning
[Quality trend](quality-trend.md) already gives for a dedicated branch over
`gh-pages`: incremental, no publish-cadence coupling, fetchable client-side with no
auth). On top of the hard gate's absolute 32ms budget, it applies a trailing-baseline
check: a soft one (20% slower than the trailing 10-run mean, `::warning::` only) and
a hard one (100% slower - i.e. at least doubled - `::error::`, fails the
`persist-performance-trend` CI job *after* the numbers are still recorded, so a big
regression is never silently un-recorded just because it also failed the run).

`tools/ci/append_kernel_history.py` does the same for `ac3kernelbench`'s per-kernel
numbers (`kernels-develop.jsonl` / `kernels-main.jsonl`, same branch), with the same
two trailing-baseline tiers - but both tiers are `::warning::` annotations and the
kernel series **never fails the job**: a micro-kernel's ns/call on a shared CI runner
is far noisier than a 200-frame whole-frame average, and `ac3perf` plus the
whole-frame series already gate anything a user would feel. This page's per-kernel
tables are where kernel regressions surface. Every series is keyed by its own kernel
name end to end - a trailing mean over mixed kernels would be a number with no owner,
the same conflation the quality-trend history once had to be cured of.

`tools/ci/append_memory_history.py` does the same for `ac3membench`'s numbers
(`memory-develop.jsonl` / `memory-main.jsonl`, same branch), with the same two
tiers on **two** churn metrics per series - allocations/frame and bytes/frame,
either one regressing flags the record - and it gates like the whole-frame
series does (the hard tier fails the job, after the push). One check is absolute
rather than trend-relative: `steady_live_growth`, the bytes still held live
after ~200 steady-state frames, warns above 4 KiB and hard-fails above 1 MiB,
because a leak is a leak regardless of what last week's runs did.

Only `linux-gcc` is measured, not the full CI matrix: a timing trend's value is in
comparing one consistent runner against its own history over time, not in comparing
GitHub's runner classes against each other the way the gold-reference SNR numbers
usefully are cross-platform.

## Whole-frame trend

Each series below is a chart first, table second. The chart plots that
series' *entire* recorded history (not just the table's last 20 rows) as
ms/frame against commit date, with a dashed red line for the throughput
budget and a dashed ring around any point whose commit was tagged as a
GitHub release - hover a point for the exact commit, date and number. A
downward slope is the improvement this whole page exists to make visible;
release rings turn that into a release-to-release story instead of a wall
of per-commit numbers.

<div id="performance-trend-app">
  <p class="performance-trend-status">Loading trend data…</p>
</div>

<style>
#performance-trend-app { margin: 1.5em 0; }
.performance-trend-status { color: var(--md-default-fg-color--light); font-style: italic; }
.performance-trend-chart-wrap { overflow-x: auto; margin-bottom: 0.5em; }
.performance-trend-chart { display: block; }
.performance-trend-legend { display: flex; gap: 1.5em; font-size: 0.8em; margin: 0.25em 0 1em; color: var(--md-default-fg-color--light); flex-wrap: wrap; }
.performance-trend-legend span { display: inline-flex; align-items: center; gap: 0.4em; }
.performance-trend-legend i { width: 0.9em; height: 0.9em; border-radius: 50%; display: inline-block; background: #7c4dff; }
.performance-trend-table-wrap { overflow-x: auto; }
#performance-trend-app table { width: 100%; border-collapse: collapse; font-size: 0.85em; }
#performance-trend-app th, #performance-trend-app td { padding: 0.35em 0.6em; text-align: left; border-bottom: 1px solid var(--md-default-fg-color--lightest); white-space: nowrap; }
.performance-trend-over-budget { color: var(--md-typeset-mark-color, #c62828); font-weight: 600; }
.performance-trend-branch-heading { margin-top: 1.5em; }
.performance-trend-release-row { background: color-mix(in srgb, var(--md-accent-fg-color, #7c4dff) 8%, transparent); }
.performance-trend-release { text-decoration: none; font-weight: 600; }
.performance-trend-release:hover { text-decoration: underline; }
</style>

<script>
(function () {
  const REPO = "iainchesworthlabs/ac3forge";
  const HISTORY_BRANCH = "quality-history";
  const BRANCHES = ["develop", "main"];
  // How many of each (branch, leg, config)'s most recent rows to show in the
  // table - a trend readout, not a full audit log. Mirrors quality-trend.md's
  // own TABLE_ROWS in spirit, just scoped per-series instead of globally,
  // since performance-trend.md only ever has one leg (linux-gcc) rather than
  // quality-trend's five. The chart above each table is NOT capped to this -
  // it plots the series' full history so a release from further back than
  // the last 20 runs still shows up as a ring.
  const ROWS_PER_SERIES = 20;
  const CHART_COLOR = "#7c4dff";

  const root = document.getElementById("performance-trend-app");

  function rawUrl(branch, file) {
    return `https://raw.githubusercontent.com/${REPO}/${branch}/${file}`;
  }

  function parseJsonl(text) {
    return text.split("\n").filter((l) => l.trim().length > 0).map((l) => JSON.parse(l));
  }

  async function fetchBranch(branch) {
    try {
      const resp = await fetch(rawUrl(HISTORY_BRANCH, `performance-${branch}.jsonl`));
      if (!resp.ok) return [];
      return parseJsonl(await resp.text());
    } catch (e) {
      return [];
    }
  }

  // Same best-effort tag->commit join quality-trend.md already relies on:
  // release.yml tags an existing main commit after the fact, so the tag
  // never appears in the performance-history record for that commit itself -
  // this is a client-side join against the GitHub API, not a second data
  // source. A rate-limited or offline response just means no release
  // markers, not a broken page.
  async function fetchReleaseShaMap() {
    let shaMap = {};
    try {
      const resp = await fetch(`https://api.github.com/repos/${REPO}/tags?per_page=100`);
      if (!resp.ok) return shaMap;
      const tags = await resp.json();
      for (const t of tags) {
        if (t.commit && t.commit.sha) shaMap[t.commit.sha] = { tag: t.name, name: t.name, url: `https://github.com/${REPO}/releases/tag/${t.name}` };
      }
    } catch (e) {
      return shaMap;
    }
    try {
      const resp = await fetch(`https://api.github.com/repos/${REPO}/releases?per_page=100`);
      if (resp.ok) {
        const releases = await resp.json();
        const byTag = {};
        for (const rel of releases) byTag[rel.tag_name] = rel;
        for (const sha of Object.keys(shaMap)) {
          const rel = byTag[shaMap[sha].tag];
          if (rel) {
            shaMap[sha].name = rel.name || shaMap[sha].tag;
            shaMap[sha].prerelease = !!rel.prerelease;
            shaMap[sha].url = rel.html_url || shaMap[sha].url;
          }
        }
      }
    } catch (e) {
      // Tag->sha map still usable without release metadata.
    }
    return shaMap;
  }

  function shortSha(sha) {
    return (sha || "").slice(0, 7);
  }

  function groupBy(records, keyFn) {
    const groups = new Map();
    for (const rec of records) {
      const key = keyFn(rec);
      if (!groups.has(key)) groups.set(key, []);
      groups.get(key).push(rec);
    }
    return groups;
  }

  // sortedRows: full series history, oldest to newest. Budget is read off
  // the most recent row, same as the table below - a config's budget
  // doesn't change often, but this always reflects the current one, not a
  // stale first-run value.
  function buildChart(sortedRows, releasesBySha) {
    const width = 720, height = 160, pad = { top: 10, right: 12, bottom: 22, left: 46 };
    const budget = sortedRows.length ? sortedRows[sortedRows.length - 1].real_time_budget_ms_per_frame : null;
    const values = sortedRows.map((r) => r.ms_per_frame).concat(budget !== null ? [budget] : []);
    const minMs = Math.min(...values) * 0.95;
    const maxMs = Math.max(...values) * 1.05;
    const times = sortedRows.map((r) => Date.parse(r.commit_date));
    const minT = Math.min(...times);
    const maxT = Math.max(...times);

    const x = (t) => pad.left + (maxT === minT ? (width - pad.left - pad.right) / 2 : ((t - minT) / (maxT - minT)) * (width - pad.left - pad.right));
    const y = (ms) => height - pad.bottom - ((ms - minMs) / (maxMs - minMs)) * (height - pad.top - pad.bottom);

    let svg = `<svg class="performance-trend-chart" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}" role="img" aria-label="ms per frame by commit date">`;
    for (let i = 0; i <= 3; i++) {
      const ms = minMs + ((maxMs - minMs) * i) / 3;
      const gy = y(ms);
      svg += `<line x1="${pad.left}" y1="${gy}" x2="${width - pad.right}" y2="${gy}" stroke="var(--md-default-fg-color--lightest)" stroke-width="1"/>`;
      svg += `<text x="${pad.left - 6}" y="${gy + 3}" text-anchor="end" font-size="10" fill="var(--md-default-fg-color--light)">${ms.toFixed(2)}</text>`;
    }
    if (budget !== null) {
      const by = y(budget);
      svg += `<line x1="${pad.left}" y1="${by}" x2="${width - pad.right}" y2="${by}" stroke="var(--md-typeset-mark-color, #c62828)" stroke-width="1" stroke-dasharray="4,3"><title>Budget: ${budget.toFixed(3)} ms/frame</title></line>`;
    }
    svg += `<text x="${pad.left}" y="${height - 6}" text-anchor="start" font-size="10" fill="var(--md-default-fg-color--light)">${new Date(minT).toISOString().slice(0, 10)}</text>`;
    svg += `<text x="${width - pad.right}" y="${height - 6}" text-anchor="end" font-size="10" fill="var(--md-default-fg-color--light)">${new Date(maxT).toISOString().slice(0, 10)}</text>`;

    if (sortedRows.length > 1) {
      const path = sortedRows.map((r, i) => `${i === 0 ? "M" : "L"}${x(Date.parse(r.commit_date)).toFixed(1)},${y(r.ms_per_frame).toFixed(1)}`).join(" ");
      svg += `<path d="${path}" fill="none" stroke="${CHART_COLOR}" stroke-width="2"/>`;
    }
    sortedRows.forEach((r) => {
      const cx = x(Date.parse(r.commit_date)).toFixed(1);
      const cy = y(r.ms_per_frame).toFixed(1);
      const release = releasesBySha[r.commit];
      const title = `${shortSha(r.commit)} - ${r.ms_per_frame.toFixed(3)} ms/frame on ${r.commit_date.slice(0, 10)}${release ? ` - release ${release.name}` : ""}`;
      svg += `<circle cx="${cx}" cy="${cy}" r="3" fill="${CHART_COLOR}"><title>${title}</title></circle>`;
      if (release) {
        svg += `<circle cx="${cx}" cy="${cy}" r="6.5" fill="none" stroke="${CHART_COLOR}" stroke-width="1.5" stroke-dasharray="2,1.5"><title>${title}</title></circle>`;
      }
    });
    svg += "</svg>";
    return svg;
  }

  function buildLegend(sortedRows, releasesBySha) {
    const anyRelease = sortedRows.some((r) => releasesBySha[r.commit]);
    const items = [
      '<span><i></i>ms/frame</span>',
      '<span><i style="background:none;border:1.5px dashed var(--md-typeset-mark-color, #c62828);border-radius:0;"></i>budget</span>',
    ];
    if (anyRelease) {
      items.push('<span><i style="background:none;border:1.5px dashed var(--md-default-fg-color--light);"></i>tagged release</span>');
    }
    return `<div class="performance-trend-legend">${items.join("")}</div>`;
  }

  function renderSeries(leg, config, rows, releasesBySha) {
    const sorted = rows.slice().sort((a, b) => a.commit_date.localeCompare(b.commit_date));
    const recent = sorted.slice(-ROWS_PER_SERIES).reverse();
    const budget = sorted.length ? sorted[sorted.length - 1].real_time_budget_ms_per_frame : null;
    const trs = recent.map((r) => {
      const overBudget = budget !== null && r.ms_per_frame > budget;
      const release = releasesBySha[r.commit];
      const classes = [overBudget ? "performance-trend-over-budget" : "", release ? "performance-trend-release-row" : ""].filter(Boolean).join(" ");
      const releaseBadge = release
        ? `<a class="performance-trend-release" href="${release.url}" title="${release.prerelease ? "Prerelease" : "Release"} tagged at this commit">🏷 ${release.name}</a>`
        : "";
      return `<tr${classes ? ` class="${classes}"` : ""}>
        <td>${r.commit_date ? r.commit_date.slice(0, 10) : ""}</td>
        <td><a href="https://github.com/${REPO}/commit/${r.commit}">${shortSha(r.commit)}</a></td>
        <td>${r.ms_per_frame.toFixed(3)}</td>
        <td>${budget !== null ? budget.toFixed(3) : ""}</td>
        <td>${r.frames}</td>
        <td>${releaseBadge}</td>
      </tr>`;
    }).join("");

    return `<h4>${leg} / ${config}</h4>
    <div class="performance-trend-chart-wrap">${buildChart(sorted, releasesBySha)}</div>
    ${buildLegend(sorted, releasesBySha)}
    <div class="performance-trend-table-wrap">
      <table>
        <thead><tr><th>Date</th><th>Commit</th><th>ms/frame</th><th>Budget (ms/frame)</th><th>Frames</th><th>Release</th></tr></thead>
        <tbody>${trs}</tbody>
      </table>
    </div>`;
  }

  function renderBranch(branch, records, releasesBySha) {
    if (records.length === 0) {
      return `<h3 class="performance-trend-branch-heading">${branch}</h3><p><em>No data yet.</em></p>`;
    }
    const groups = groupBy(records, (r) => `${r.leg} ${r.config}`);
    const sections = [...groups.entries()]
      .sort((a, b) => a[0].localeCompare(b[0]))
      .map(([key, rows]) => {
        const [leg, config] = key.split(" ");
        return renderSeries(leg, config, rows, releasesBySha);
      })
      .join("\n");
    return `<h3 class="performance-trend-branch-heading">${branch}</h3>${sections}`;
  }

  async function render() {
    const [perBranch, releasesBySha] = await Promise.all([
      Promise.all(BRANCHES.map(fetchBranch)),
      fetchReleaseShaMap(),
    ]);
    const anyData = perBranch.some((records) => records.length > 0);
    if (!anyData) {
      root.innerHTML = '<p class="performance-trend-status">No performance-trend data recorded yet - it appears after the first develop/main push that reaches the persist-performance-trend CI job.</p>';
      return;
    }
    root.innerHTML = BRANCHES.map((branch, i) => renderBranch(branch, perBranch[i], releasesBySha)).join("\n");
  }

  render();
})();
</script>

## Per-kernel trend

Same commits, one level finer: each kernel's ns/call from `ac3kernelbench`, one
series per kernel. The Δ column is each run against its own series' trailing
10-run mean - the same window and thresholds `append_kernel_history.py` annotates
with: ≥ +20% is flagged as a soft drift, ≥ +100% as a hard one. Neither ever fails
CI (see above); a flagged row here is an invitation to look, not a broken build.

<div id="kernel-trend-app">
  <p class="performance-trend-status">Loading kernel trend data…</p>
</div>

<style>
#kernel-trend-app { margin: 1.5em 0; }
#kernel-trend-app table { width: 100%; border-collapse: collapse; font-size: 0.85em; }
#kernel-trend-app th, #kernel-trend-app td { padding: 0.35em 0.6em; text-align: left; border-bottom: 1px solid var(--md-default-fg-color--lightest); white-space: nowrap; }
.kernel-trend-soft { color: var(--md-warning-fg-color, #e65100); font-weight: 600; }
.kernel-trend-hard { color: var(--md-typeset-mark-color, #c62828); font-weight: 600; }
</style>

<script>
(function () {
  const REPO = "iainchesworthlabs/ac3forge";
  const HISTORY_BRANCH = "quality-history";
  const BRANCHES = ["develop", "main"];
  // Fewer rows per series than the whole-frame tables' 20: there are a dozen-plus
  // kernel series to the whole-frame tables' handful of configs, and this page is
  // a trend readout, not an audit log - the JSONL keeps everything.
  const ROWS_PER_SERIES = 10;
  // Mirrors append_kernel_history.py's REGRESSION_TRAILING_WINDOW and its two
  // annotation tiers, so a flagged row here and a ::warning:: in the CI log are
  // the same statement about the same numbers.
  const TRAILING_WINDOW = 10;
  const SOFT_FRACTION = 0.20;
  const HARD_FRACTION = 1.0;

  const root = document.getElementById("kernel-trend-app");

  function rawUrl(branch, file) {
    return `https://raw.githubusercontent.com/${REPO}/${branch}/${file}`;
  }

  function parseJsonl(text) {
    return text.split("\n").filter((l) => l.trim().length > 0).map((l) => JSON.parse(l));
  }

  async function fetchBranch(branch) {
    try {
      const resp = await fetch(rawUrl(HISTORY_BRANCH, `kernels-${branch}.jsonl`));
      if (!resp.ok) return [];
      return parseJsonl(await resp.text());
    } catch (e) {
      return [];
    }
  }

  function shortSha(sha) {
    return (sha || "").slice(0, 7);
  }

  function groupBy(records, keyFn) {
    const groups = new Map();
    for (const rec of records) {
      const key = keyFn(rec);
      if (!groups.has(key)) groups.set(key, []);
      groups.get(key).push(rec);
    }
    return groups;
  }

  function formatNs(ns) {
    return ns >= 100 ? ns.toFixed(0) : ns.toFixed(1);
  }

  function renderSeries(leg, kernel, rows) {
    // Each row's baseline is the trailing mean of the rows BEFORE it - the
    // same the-append-script-saw-it semantics as the CI annotations, which
    // compute the baseline before appending the new record.
    const annotated = rows.map((r, i) => {
      const tail = rows.slice(Math.max(0, i - TRAILING_WINDOW), i).map((p) => p.ns_per_call);
      const baseline = tail.length ? tail.reduce((a, b) => a + b, 0) / tail.length : null;
      const slowdown = baseline && baseline > 0 ? (r.ns_per_call - baseline) / baseline : null;
      return { ...r, slowdown };
    });
    const recent = annotated.slice(-ROWS_PER_SERIES).reverse();
    const trs = recent.map((r) => {
      let cls = "";
      if (r.slowdown !== null && r.slowdown >= HARD_FRACTION) cls = ' class="kernel-trend-hard"';
      else if (r.slowdown !== null && r.slowdown >= SOFT_FRACTION) cls = ' class="kernel-trend-soft"';
      const delta = r.slowdown === null ? "" : `${r.slowdown >= 0 ? "+" : ""}${(r.slowdown * 100).toFixed(1)}%`;
      return `<tr${cls}>
        <td>${r.commit_date ? r.commit_date.slice(0, 10) : ""}</td>
        <td><a href="https://github.com/${REPO}/commit/${r.commit}">${shortSha(r.commit)}</a></td>
        <td>${formatNs(r.ns_per_call)}</td>
        <td>${delta}</td>
        <td>${r.iters}</td>
      </tr>`;
    }).join("");

    return `<h4>${leg} / ${kernel}</h4>
    <div class="performance-trend-table-wrap">
      <table>
        <thead><tr><th>Date</th><th>Commit</th><th>ns/call</th><th>Δ vs trailing mean</th><th>Iters</th></tr></thead>
        <tbody>${trs}</tbody>
      </table>
    </div>`;
  }

  function renderBranch(branch, records) {
    if (records.length === 0) {
      return `<h3 class="performance-trend-branch-heading">${branch}</h3><p><em>No data yet.</em></p>`;
    }
    // Grouped per (leg, kernel), never merged across kernels - a series is
    // only meaningful against its own history.
    const groups = groupBy(records, (r) => `${r.leg} ${r.kernel}`);
    const sections = [...groups.entries()]
      .sort((a, b) => a[0].localeCompare(b[0]))
      .map(([key, rows]) => {
        const [leg, kernel] = key.split(" ");
        return renderSeries(leg, kernel, rows);
      })
      .join("\n");
    return `<h3 class="performance-trend-branch-heading">${branch}</h3>${sections}`;
  }

  async function render() {
    const perBranch = await Promise.all(BRANCHES.map(fetchBranch));
    const anyData = perBranch.some((records) => records.length > 0);
    if (!anyData) {
      root.innerHTML = '<p class="performance-trend-status">No kernel-trend data recorded yet - it appears after the first develop/main push that reaches the persist-performance-trend CI job.</p>';
      return;
    }
    root.innerHTML = BRANCHES.map((branch, i) => renderBranch(branch, perBranch[i])).join("\n");
  }

  render();
})();
</script>

## Memory trend

Same commits, a different resource: each workload's heap-allocation count and
allocator traffic per frame from `ac3membench`, one series per workload -
including the decode paths the timing benches don't cover. The Δ column is
bytes/frame against the series' trailing 10-run mean, the same window and
thresholds `append_memory_history.py` gates with (≥ +20% soft, ≥ +100% hard on
*either* churn metric); a non-zero **live growth** is its own signal (bytes
still held after ~200 steady-state frames - the leak check is absolute, not
trend-relative). These counts are near-deterministic for a fixed workload: a
flagged row is a real change in allocation behaviour, not runner noise. The
memory-usage optimization programme's phases land as visible downward steps in
these series - that is what this table exists to show.

<div id="memory-trend-app">
  <p class="performance-trend-status">Loading memory trend data…</p>
</div>

<style>
#memory-trend-app { margin: 1.5em 0; }
#memory-trend-app table { width: 100%; border-collapse: collapse; font-size: 0.85em; }
#memory-trend-app th, #memory-trend-app td { padding: 0.35em 0.6em; text-align: left; border-bottom: 1px solid var(--md-default-fg-color--lightest); white-space: nowrap; }
</style>

<script>
(function () {
  const REPO = "iainchesworthlabs/ac3forge";
  const HISTORY_BRANCH = "quality-history";
  const BRANCHES = ["develop", "main"];
  const ROWS_PER_SERIES = 10;
  // Mirrors append_memory_history.py's window and tiers, so a flagged row
  // here and an annotation in the CI log are the same statement about the
  // same numbers. The flag is on the WORSE of the two churn metrics.
  const TRAILING_WINDOW = 10;
  const SOFT_FRACTION = 0.20;
  const HARD_FRACTION = 1.0;

  const root = document.getElementById("memory-trend-app");

  function rawUrl(branch, file) {
    return `https://raw.githubusercontent.com/${REPO}/${branch}/${file}`;
  }

  function parseJsonl(text) {
    return text.split("\n").filter((l) => l.trim().length > 0).map((l) => JSON.parse(l));
  }

  async function fetchBranch(branch) {
    try {
      const resp = await fetch(rawUrl(HISTORY_BRANCH, `memory-${branch}.jsonl`));
      if (!resp.ok) return [];
      return parseJsonl(await resp.text());
    } catch (e) {
      return [];
    }
  }

  function shortSha(sha) {
    return (sha || "").slice(0, 7);
  }

  function groupBy(records, keyFn) {
    const groups = new Map();
    for (const rec of records) {
      const key = keyFn(rec);
      if (!groups.has(key)) groups.set(key, []);
      groups.get(key).push(rec);
    }
    return groups;
  }

  function formatBytes(b) {
    if (b >= 1024 * 1024) return `${(b / (1024 * 1024)).toFixed(1)} MiB`;
    if (b >= 1024) return `${(b / 1024).toFixed(1)} KiB`;
    return `${Math.round(b)} B`;
  }

  function metricGrowth(rows, i, metric) {
    const tail = rows.slice(Math.max(0, i - TRAILING_WINDOW), i).map((p) => p[metric]);
    const baseline = tail.length ? tail.reduce((a, b) => a + b, 0) / tail.length : null;
    if (baseline === null || baseline <= 0) return null;
    return (rows[i][metric] - baseline) / baseline;
  }

  function renderSeries(leg, config, rows) {
    // Same the-append-script-saw-it semantics as the kernel tables: each
    // row's baseline is the trailing mean of the rows BEFORE it.
    const annotated = rows.map((r, i) => {
      const allocsGrowth = metricGrowth(rows, i, "allocs_per_frame");
      const bytesGrowth = metricGrowth(rows, i, "bytes_per_frame");
      const worst = [allocsGrowth, bytesGrowth].filter((g) => g !== null)
        .reduce((a, b) => Math.max(a, b), -Infinity);
      return { ...r, bytesGrowth, worst: worst === -Infinity ? null : worst };
    });
    const recent = annotated.slice(-ROWS_PER_SERIES).reverse();
    const trs = recent.map((r) => {
      let cls = "";
      if (r.worst !== null && r.worst >= HARD_FRACTION) cls = ' class="kernel-trend-hard"';
      else if (r.worst !== null && r.worst >= SOFT_FRACTION) cls = ' class="kernel-trend-soft"';
      const delta = r.bytesGrowth === null ? "" : `${r.bytesGrowth >= 0 ? "+" : ""}${(r.bytesGrowth * 100).toFixed(1)}%`;
      return `<tr${cls}>
        <td>${r.commit_date ? r.commit_date.slice(0, 10) : ""}</td>
        <td><a href="https://github.com/${REPO}/commit/${r.commit}">${shortSha(r.commit)}</a></td>
        <td>${r.allocs_per_frame.toFixed(1)}</td>
        <td>${formatBytes(r.bytes_per_frame)}</td>
        <td>${r.steady_live_growth === 0 ? "0" : formatBytes(r.steady_live_growth)}</td>
        <td>${delta}</td>
        <td>${formatBytes(r.peak_rss_bytes)}</td>
      </tr>`;
    }).join("");

    return `<h4>${leg} / ${config}</h4>
    <div class="performance-trend-table-wrap">
      <table>
        <thead><tr><th>Date</th><th>Commit</th><th>Allocs/frame</th><th>Bytes/frame</th><th>Live growth</th><th>Δ bytes vs trailing mean</th><th>Peak RSS</th></tr></thead>
        <tbody>${trs}</tbody>
      </table>
    </div>`;
  }

  function renderBranch(branch, records) {
    if (records.length === 0) {
      return `<h3 class="performance-trend-branch-heading">${branch}</h3><p><em>No data yet.</em></p>`;
    }
    const groups = groupBy(records, (r) => `${r.leg} ${r.config}`);
    const sections = [...groups.entries()]
      .sort((a, b) => a[0].localeCompare(b[0]))
      .map(([key, rows]) => {
        const [leg, config] = key.split(" ");
        return renderSeries(leg, config, rows);
      })
      .join("\n");
    return `<h3 class="performance-trend-branch-heading">${branch}</h3>${sections}`;
  }

  async function render() {
    const perBranch = await Promise.all(BRANCHES.map(fetchBranch));
    const anyData = perBranch.some((records) => records.length > 0);
    if (!anyData) {
      root.innerHTML = '<p class="performance-trend-status">No memory-trend data recorded yet - it appears after the first develop/main push that reaches the persist-performance-trend CI job.</p>';
      return;
    }
    root.innerHTML = BRANCHES.map((branch, i) => renderBranch(branch, perBranch[i])).join("\n");
  }

  render();
})();
</script>
