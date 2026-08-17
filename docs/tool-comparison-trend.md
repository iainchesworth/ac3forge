# Tool comparison trend

The commit-level half of the external-encoder landscape comparison — see
[Landscape](landscape.md) for the release-facing headline number. Every push
to `develop` or `main` encodes the same three fixed legs
[`tools/gen_external_baseline.py`](https://github.com/iainchesworthlabs/ac3forge/blob/main/tools/gen_external_baseline.py)
measures FFmpeg's and Dolby DEE's encoders against, scores this build's own
output through `ac3cli`'s own decoder (no FFmpeg, no DEE, at CI time — see
`tools/quality_race.py`'s `trend` mode), and appends the numbers here. It
exists to answer a narrower question than the landscape page: not "are we
competitive with the outside world" but "did this specific Annex E tool get
better or worse as the code changed" — one row per (leg, tool-set), not just
the single black-box number a real user actually gets.

The `landscape` row is E-AC-3's `auto` tools — the set this encoder picks
from the per-channel rate, and so the configuration comparable to FFmpeg's/
DEE's own automatic best-effort choices (AC-3 has no such toggle; coupling,
rematrixing and delta bit allocation are unconditionally automatic there).
It is the same encode as the `auto` variant row, which is why those two
lines coincide exactly; the remaining rows are the forced sets `auto` is
choosing between, which is what makes the cost of each one legible here.
It carries `vs_ffmpeg`/`vs_dee` columns —
the delta against the committed baseline's numbers for the *same* leg — that
the other rows don't, since only `landscape` has a matching external number
to compare against. A leg whose DEE score is still marked unverified in
[`tests/golden/external-baseline/manifest.json`](https://github.com/iainchesworthlabs/ac3forge/blob/main/tests/golden/external-baseline/manifest.json)
(see that file's own header) shows no `vs_dee` value rather than one
computed against a number that was never real.

Same two-tier regression check as [Quality trend](quality-trend.md): a soft
one (0.5 dB below the trailing 10-run mean for the same leg/variant/branch)
that only annotates a row, and a hard one (10 dB below) that fails the CI run
that produced it — after the numbers are still recorded here. See
`REGRESSION_DROP_DB`/`HARD_REGRESSION_DROP_DB` in
`scripts/append-external-comparison-history.py`.

**MOS** is a perceptual-quality prediction alongside the waveform-level SNR —
[ViSQOL](https://github.com/google/visqol)'s MOS-LQO (Mean Opinion Score -
Listening Quality Objective) in audio mode, 1 (bad) to a ceiling around 4.75,
via the `visqol-python` package (see `perceptual_score()` in
`tools/quality_race.py` for why ViSQOL over PEAQ and why that package
specifically). It's read-only here — no regression check, unlike SNR — and
shows `-` on any row a CI run produced without `visqol-python` installed in
that job's environment, not a real zero score.

<div id="tool-trend-app">
  <p class="tool-trend-status">Loading trend data…</p>
</div>

<style>
#tool-trend-app { margin: 1.5em 0; }
.tool-trend-status { color: var(--md-default-fg-color--light); font-style: italic; }
.tool-trend-controls { display: flex; gap: 1.25em; align-items: center; margin-bottom: 0.75em; flex-wrap: wrap; }
.tool-trend-controls label { font-size: 0.85em; color: var(--md-default-fg-color--light); display: inline-flex; align-items: center; gap: 0.35em; white-space: nowrap; }
.tool-trend-controls select {
  font: inherit; padding: 0.2em 0.5em; border-radius: 0.2em;
  border: 1px solid var(--md-default-fg-color--lightest);
  background: var(--md-default-bg-color); color: var(--md-default-fg-color);
}
.tool-trend-variant-filter { display: flex; gap: 0.75em; flex-wrap: wrap; font-size: 0.85em; margin-bottom: 0.75em; }
.tool-trend-variant-filter label { color: var(--md-default-fg-color--light); display: inline-flex; align-items: center; gap: 0.3em; }
.tool-trend-chart-wrap { overflow-x: auto; margin-bottom: 1em; }
.tool-trend-chart { display: block; }
.tool-trend-legend { display: flex; gap: 1.5em; font-size: 0.85em; margin: 0.5em 0 1em; flex-wrap: wrap; }
.tool-trend-legend span { display: inline-flex; align-items: center; gap: 0.4em; }
.tool-trend-legend i { width: 0.9em; height: 0.9em; border-radius: 50%; display: inline-block; }
.tool-trend-table-wrap { overflow-x: auto; }
#tool-trend-app table { width: 100%; border-collapse: collapse; font-size: 0.85em; }
#tool-trend-app th, #tool-trend-app td { padding: 0.35em 0.6em; text-align: left; border-bottom: 1px solid var(--md-default-fg-color--lightest); white-space: nowrap; }
.tool-trend-regression { color: var(--md-typeset-mark-color, #c62828); font-weight: 600; }
.tool-trend-release-row { background: color-mix(in srgb, var(--md-accent-fg-color, #7c4dff) 8%, transparent); }
.tool-trend-release { text-decoration: none; font-weight: 600; }
.tool-trend-release:hover { text-decoration: underline; }
.tool-trend-delta-up { color: #2e7d32; }
.tool-trend-delta-down { color: #c62828; }
</style>

<script>
(function () {
  const REPO = "iainchesworthlabs/ac3forge";
  const HISTORY_BRANCH = "quality-history";
  const TRACKS = [
    { branch: "develop", color: "#7c4dff" },
    { branch: "main", color: "#00acc1" },
  ];
  // Mirrors scripts/append-external-comparison-history.py's own constants -
  // keep these in sync if that script's thresholds change; this is a
  // display-only echo, not a second source of truth. Only the soft
  // (non-failing) tier is shown here - a hard regression fails its own CI
  // run directly, so it doesn't need a table annotation to be noticed too.
  const REGRESSION_WINDOW = 10;
  const REGRESSION_DROP_DB = 0.5;
  const TABLE_ROWS = 40;
  const LEGS = ["ac3-51-448", "eac3-stereo-192", "eac3-51-256"];
  // Every variant tools/quality_race.py's `trend` mode can emit - see
  // EAC3_VARIANTS/EAC3_SELF_VARIANTS there. AC-3's only row is "landscape"
  // (no tool tokens exist for it); the others simply never appear for that
  // leg, so the checkboxes below are the same list regardless of which leg
  // is selected, and rows for a variant the leg doesn't have just don't show.
  const ALL_VARIANTS = ["landscape", "none", "auto", "cpl", "spx", "aht", "cpl+spx", "all", "ecpl", "tpn", "ecpl+tpn"];
  // Fixed color per variant, same reasoning as quality-trend.md's LEGS - a
  // variant's line keeps the same color across renders instead of shuffling
  // with whichever variants happen to have data for the selected leg (AC-3's
  // only variant is "landscape"; the rest just have no points there).
  const VARIANT_COLORS = {
    landscape: "#7c4dff",
    none: "#00acc1",
    auto: "#8d6e63",
    cpl: "#43a047",
    spx: "#fb8c00",
    aht: "#e53935",
    "cpl+spx": "#3949ab",
    all: "#d81b60",
    ecpl: "#6d4c41",
    tpn: "#00897b",
    "ecpl+tpn": "#757575",
  };
  // The chart plots one metric line per branch for a single focus variant -
  // same reasoning as quality-trend.md's own chart (a line per leg AND per
  // variant would be unreadable) - while the table below can show every
  // checked variant's full detail at once.
  const DEFAULT_TABLE_VARIANTS = ["landscape", "none", "all"];

  const root = document.getElementById("tool-trend-app");

  const state = {
    leg: "eac3-stereo-192",
    // "branch": one line per branch for a single focus variant (the
    // original view). "variant": one line per tool-set variant for a single
    // branch, un-folded - same branch-vs-leg tradeoff as quality-trend.md's
    // "By platform leg" view: never both branch and variant as line
    // dimensions at once, since up to 10 variants x 2 branches would be as
    // unreadable as the per-leg-and-variant chart this page already avoids.
    chartMode: "branch",
    chartVariant: "landscape",
    // Which branch's per-variant lines are drawn in "variant" mode. develop
    // by default, same reasoning as quality-trend.md's legBranch.
    chartBranch: "develop",
    tableVariants: Object.fromEntries(ALL_VARIANTS.map((v) => [v, DEFAULT_TABLE_VARIANTS.includes(v)])),
    branches: { main: true, develop: true },
    developFullHistory: false,
  };

  function rawUrl(branch, file) {
    return `https://raw.githubusercontent.com/${REPO}/${branch}/${file}`;
  }

  function parseJsonl(text) {
    return text.split("\n").filter((l) => l.trim().length > 0).map((l) => JSON.parse(l));
  }

  async function fetchTrack(branch) {
    try {
      const resp = await fetch(rawUrl(HISTORY_BRANCH, `external-comparison-${branch}.jsonl`));
      if (!resp.ok) return [];
      return parseJsonl(await resp.text());
    } catch (e) {
      return [];
    }
  }

  // Same client-side, best-effort tag->release join as quality-trend.md -
  // see that page's identical function for the full reasoning.
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
    return sha.slice(0, 8);
  }

  function commitUrl(sha) {
    return `https://github.com/${REPO}/commit/${sha}`;
  }

  function mostRecentCommit(records) {
    if (records.length === 0) return null;
    return records.reduce((a, b) => (a.commit_date > b.commit_date ? a : b)).commit;
  }

  // "variant" mode always looks at one branch's full leg history - same
  // reasoning as quality-trend.md's leg view: the point is seeing a
  // per-variant trend over many commits, so the develop-collapse and
  // other-branch filters "branch" mode uses don't apply here.
  function visibleRecords(allRecords) {
    const legRecords = allRecords.filter((r) => r.leg === state.leg);
    if (state.chartMode === "variant") {
      return legRecords.filter((r) => r.branch === state.chartBranch);
    }
    const latestDevelop = mostRecentCommit(legRecords.filter((r) => r.branch === "develop"));
    return legRecords.filter((r) => {
      if (!state.branches[r.branch]) return false;
      if (r.branch === "develop" && !state.developFullHistory) {
        return r.commit === latestDevelop;
      }
      return true;
    });
  }

  function chartSeries(records, variant) {
    return records
      .filter((r) => r.variant === variant)
      .slice()
      .sort((a, b) => a.commit_date.localeCompare(b.commit_date));
  }

  // Always against the full, unfiltered leg history - see quality-trend.md's
  // identical function for why the display collapse shouldn't change what
  // counts as a regression.
  function regressionBaseline(allLegRecords, variant, branch, beforeCommitDate) {
    const trail = allLegRecords
      .filter((r) => r.variant === variant && r.branch === branch && r.commit_date < beforeCommitDate)
      .sort((a, b) => a.commit_date.localeCompare(b.commit_date))
      .slice(-REGRESSION_WINDOW);
    if (trail.length === 0) return null;
    return trail.reduce((sum, r) => sum + r.snr_db, 0) / trail.length;
  }

  // Generic over what a "track" is: branch mode passes one track per branch
  // for the focus variant, variant mode passes one track per tool-set
  // variant for a single branch - same x/y math and SVG either way, only the
  // line count and labels differ. Each track is { key, label, color, points
  // } - see quality-trend.md's identical buildChart for the same pattern.
  function buildChart(tracks, releasesBySha) {
    const width = 760, height = 220, pad = { top: 12, right: 12, bottom: 32, left: 42 };
    const allPoints = tracks.flatMap((t) => t.points);
    if (allPoints.length === 0) {
      return state.chartMode === "variant"
        ? `<p class="tool-trend-status">No history for ${state.leg} on ${state.chartBranch} in the current view.</p>`
        : `<p class="tool-trend-status">No "${state.chartVariant}" history for ${state.leg} in the current view.</p>`;
    }
    const dbValues = allPoints.map((p) => p.snr_db);
    const minDb = Math.min(...dbValues) - 2;
    const maxDb = Math.max(...dbValues) + 2;
    const times = allPoints.map((p) => Date.parse(p.commit_date));
    const minT = Math.min(...times);
    const maxT = Math.max(...times);

    const x = (t) => pad.left + (maxT === minT ? (width - pad.left - pad.right) / 2 : ((t - minT) / (maxT - minT)) * (width - pad.left - pad.right));
    const y = (db) => height - pad.bottom - ((db - minDb) / (maxDb - minDb)) * (height - pad.top - pad.bottom);

    const chartLabel = state.chartMode === "variant"
      ? `SNR by commit date, ${state.leg} on ${state.chartBranch}, by variant`
      : `SNR by commit date, ${state.leg} ${state.chartVariant}`;
    let svg = `<svg class="tool-trend-chart" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}" role="img" aria-label="${chartLabel}">`;
    for (let i = 0; i <= 4; i++) {
      const db = minDb + ((maxDb - minDb) * i) / 4;
      const gy = y(db);
      svg += `<line x1="${pad.left}" y1="${gy}" x2="${width - pad.right}" y2="${gy}" stroke="var(--md-default-fg-color--lightest)" stroke-width="1"/>`;
      svg += `<text x="${pad.left - 6}" y="${gy + 3}" text-anchor="end" font-size="10" fill="var(--md-default-fg-color--light)">${db.toFixed(0)}</text>`;
    }
    svg += `<text x="${pad.left}" y="${height - 8}" text-anchor="start" font-size="10" fill="var(--md-default-fg-color--light)">${new Date(minT).toISOString().slice(0, 10)}</text>`;
    svg += `<text x="${width - pad.right}" y="${height - 8}" text-anchor="end" font-size="10" fill="var(--md-default-fg-color--light)">${new Date(maxT).toISOString().slice(0, 10)}</text>`;

    for (const track of tracks) {
      const pts = track.points;
      if (pts.length === 0) continue;
      if (pts.length > 1) {
        const path = pts.map((p, i) => `${i === 0 ? "M" : "L"}${x(Date.parse(p.commit_date)).toFixed(1)},${y(p.snr_db).toFixed(1)}`).join(" ");
        svg += `<path d="${path}" fill="none" stroke="${track.color}" stroke-width="2"/>`;
      }
      pts.forEach((p) => {
        const cx = x(Date.parse(p.commit_date)).toFixed(1);
        const cy = y(p.snr_db).toFixed(1);
        const release = releasesBySha[p.commit];
        const title = `${track.label} ${shortSha(p.commit)} - ${p.snr_db.toFixed(2)} dB on ${p.commit_date.slice(0, 10)}${release ? ` - release ${release.name}` : ""}`;
        svg += `<circle cx="${cx}" cy="${cy}" r="3" fill="${track.color}"><title>${title}</title></circle>`;
        if (release) {
          svg += `<circle cx="${cx}" cy="${cy}" r="6.5" fill="none" stroke="${track.color}" stroke-width="1.5" stroke-dasharray="2,1.5"><title>${title}</title></circle>`;
        }
      });
    }
    svg += "</svg>";
    return svg;
  }

  // Filters out tracks with no points rather than trusting the caller's
  // list - branch mode already sets an unchecked branch's points to [], and
  // variant mode's ALL_VARIANTS list includes variants the selected leg
  // never emits (AC-3 only has "landscape"), so this is the one place both
  // cases collapse to "don't show a swatch for a line nothing was drawn for".
  function buildLegend(tracks, releasesBySha) {
    const items = tracks.filter((t) => t.points.length > 0).map((t) => `<span><i style="background:${t.color}"></i>${t.label}</span>`);
    const anyRelease = tracks.some((t) => t.points.some((p) => releasesBySha[p.commit]));
    if (anyRelease) {
      items.push('<span><i style="background:none;border:1.5px dashed var(--md-default-fg-color--light);"></i>tagged release</span>');
    }
    return `<div class="tool-trend-legend">${items.join("")}</div>`;
  }

  function deltaCell(value) {
    if (value === undefined || value === null) return "";
    const cls = value >= 0 ? "tool-trend-delta-up" : "tool-trend-delta-down";
    return `<span class="${cls}">${value >= 0 ? "+" : ""}${value.toFixed(2)} dB</span>`;
  }

  function buildTable(visible, allLegRecords, releasesBySha) {
    const rows = visible.filter((r) => state.tableVariants[r.variant]);
    const trs = rows
      .slice()
      .sort((a, b) => b.commit_date.localeCompare(a.commit_date))
      .slice(0, TABLE_ROWS)
      .map((r) => {
        const baseline = regressionBaseline(allLegRecords, r.variant, r.branch, r.commit_date);
        const regressed = baseline !== null && baseline - r.snr_db >= REGRESSION_DROP_DB;
        const flag = regressed
          ? `<span class="tool-trend-regression" title="${(baseline - r.snr_db).toFixed(2)} dB below the trailing ${REGRESSION_WINDOW}-run mean (${baseline.toFixed(2)} dB)">▼ regression</span>`
          : "";
        const release = releasesBySha[r.commit];
        const releaseBadge = release
          ? `<a class="tool-trend-release" href="${release.url}" title="${release.prerelease ? "Prerelease" : "Release"} tagged at this commit">🏷 ${release.name}</a>`
          : "";
        return `<tr${release ? ' class="tool-trend-release-row"' : ""}>
          <td>${r.commit_date.slice(0, 10)}</td>
          <td>${r.branch}</td>
          <td><a href="${commitUrl(r.commit)}">${shortSha(r.commit)}</a></td>
          <td>${r.variant}</td>
          <td>${r.snr_db.toFixed(2)} dB</td>
          <td>${r.lsd_db === null ? "-" : r.lsd_db.toFixed(2) + " dB"}</td>
          <td>${r.mos_lqo == null ? "-" : r.mos_lqo.toFixed(2)}</td>
          <td>${deltaCell(r.vs_ffmpeg_snr_db)}</td>
          <td>${deltaCell(r.vs_dee_snr_db)}</td>
          <td>${releaseBadge}</td>
          <td>${flag}</td>
        </tr>`;
      })
      .join("");
    if (trs === "") {
      return '<p class="tool-trend-status">No rows in the current view - try a different leg/branch/variant combination.</p>';
    }
    return `<div class="tool-trend-table-wrap"><table>
      <thead><tr><th>Date</th><th>Branch</th><th>Commit</th><th>Variant</th><th>SNR</th><th>LSD</th><th>MOS</th><th>vs FFmpeg</th><th>vs DEE</th><th>Release</th><th></th></tr></thead>
      <tbody>${trs}</tbody>
    </table></div>`;
  }

  function buildControls() {
    const variantMode = state.chartMode === "variant";
    return `
      <div class="tool-trend-controls">
        <label for="tool-trend-leg">Leg
          <select id="tool-trend-leg">
            ${LEGS.map((l) => `<option value="${l}" ${state.leg === l ? "selected" : ""}>${l}</option>`).join("")}
          </select>
        </label>
        <label for="tool-trend-chart-mode">Chart
          <select id="tool-trend-chart-mode">
            <option value="branch" ${!variantMode ? "selected" : ""}>By branch</option>
            <option value="variant" ${variantMode ? "selected" : ""}>By variant</option>
          </select>
        </label>
        ${variantMode ? `
          <label for="tool-trend-chart-branch">Branch
            <select id="tool-trend-chart-branch">
              <option value="develop" ${state.chartBranch === "develop" ? "selected" : ""}>develop</option>
              <option value="main" ${state.chartBranch === "main" ? "selected" : ""}>main</option>
            </select>
          </label>
        ` : `
          <label for="tool-trend-chart-variant">Chart line
            <select id="tool-trend-chart-variant">
              ${ALL_VARIANTS.map((v) => `<option value="${v}" ${state.chartVariant === v ? "selected" : ""}>${v}</option>`).join("")}
            </select>
          </label>
          <label><input type="checkbox" id="tool-trend-branch-main" ${state.branches.main ? "checked" : ""}/> main</label>
          <label><input type="checkbox" id="tool-trend-branch-develop" ${state.branches.develop ? "checked" : ""}/> develop</label>
          ${state.branches.develop ? `<label><input type="checkbox" id="tool-trend-develop-history" ${state.developFullHistory ? "checked" : ""}/> develop: show full history</label>` : ""}
        `}
      </div>
      <div class="tool-trend-variant-filter">
        <span>Table rows:</span>
        ${ALL_VARIANTS.map((v) => `<label><input type="checkbox" class="tool-trend-variant-checkbox" data-variant="${v}" ${state.tableVariants[v] ? "checked" : ""}/> ${v}</label>`).join("")}
      </div>
    `;
  }

  function attachControlListeners(allRecords, releasesBySha) {
    document.getElementById("tool-trend-leg").addEventListener("change", (e) => {
      state.leg = e.target.value;
      render(allRecords, releasesBySha);
    });
    document.getElementById("tool-trend-chart-mode").addEventListener("change", (e) => {
      state.chartMode = e.target.value;
      render(allRecords, releasesBySha);
    });
    const chartBranchSelect = document.getElementById("tool-trend-chart-branch");
    if (chartBranchSelect) {
      chartBranchSelect.addEventListener("change", (e) => {
        state.chartBranch = e.target.value;
        render(allRecords, releasesBySha);
      });
    }
    const chartVariantSelect = document.getElementById("tool-trend-chart-variant");
    if (chartVariantSelect) {
      chartVariantSelect.addEventListener("change", (e) => {
        state.chartVariant = e.target.value;
        render(allRecords, releasesBySha);
      });
    }
    const mainToggle = document.getElementById("tool-trend-branch-main");
    if (mainToggle) {
      mainToggle.addEventListener("change", (e) => {
        state.branches.main = e.target.checked;
        render(allRecords, releasesBySha);
      });
    }
    const developToggle = document.getElementById("tool-trend-branch-develop");
    if (developToggle) {
      developToggle.addEventListener("change", (e) => {
        state.branches.develop = e.target.checked;
        render(allRecords, releasesBySha);
      });
    }
    const historyToggle = document.getElementById("tool-trend-develop-history");
    if (historyToggle) {
      historyToggle.addEventListener("change", (e) => {
        state.developFullHistory = e.target.checked;
        render(allRecords, releasesBySha);
      });
    }
    document.querySelectorAll(".tool-trend-variant-checkbox").forEach((cb) => {
      cb.addEventListener("change", (e) => {
        state.tableVariants[e.target.dataset.variant] = e.target.checked;
        render(allRecords, releasesBySha);
      });
    });
  }

  // Builds this render's tracks per state.chartMode - branch mode keeps one
  // track per branch for the focus variant (chartSeries); variant mode
  // un-folds the single chartBranch into one track per tool-set variant.
  // Mirrors quality-trend.md's identical buildTracks.
  function buildTracks(visible) {
    if (state.chartMode === "variant") {
      return ALL_VARIANTS.map((v) => ({
        key: v,
        label: v,
        color: VARIANT_COLORS[v],
        points: chartSeries(visible, v),
      }));
    }
    return TRACKS.map((t) => ({
      key: t.branch,
      label: t.branch === "develop" && !state.developFullHistory ? `${t.branch} (latest commit only)` : t.branch,
      color: t.color,
      points: state.branches[t.branch] ? chartSeries(visible.filter((r) => r.branch === t.branch), state.chartVariant) : [],
    }));
  }

  function render(allRecords, releasesBySha) {
    const allLegRecords = allRecords.filter((r) => r.leg === state.leg);
    const visible = visibleRecords(allRecords);
    const tracks = buildTracks(visible);
    root.innerHTML = `
      ${buildControls()}
      <div class="tool-trend-chart-wrap">${buildChart(tracks, releasesBySha)}</div>
      ${buildLegend(tracks, releasesBySha)}
      ${buildTable(visible, allLegRecords, releasesBySha)}
    `;
    attachControlListeners(allRecords, releasesBySha);
  }

  Promise.all([...TRACKS.map((t) => fetchTrack(t.branch)), fetchReleaseShaMap()]).then((results) => {
    const releasesBySha = results.pop();
    const allRecords = [];
    TRACKS.forEach((t, i) => allRecords.push(...results[i]));
    if (allRecords.length === 0) {
      root.innerHTML = '<p class="tool-trend-status">No tool-comparison history yet - it is written by CI on the first push to develop or main after this page landed.</p>';
      return;
    }
    render(allRecords, releasesBySha);
  });
})();
</script>

## Reading it

Each row is one (commit, leg, tool-set) result. The **Chart** control picks
what the lines represent, always within the single leg selected above. "By
branch" (the default) plots a single focus variant (`landscape` by default —
"Chart line" selector) as one point per commit per branch, against a shared
calendar x-axis — this view answers "did *this* variant regress." "By
variant" answers a different question instead: it un-folds a single branch
(picked with the **Branch** control that replaces the branch checkboxes in
this view) into one line per tool-set variant, so a variant drifting
relative to its siblings — e.g. `cpl` alone trending down while `all` holds
steady — is visible as its own line rather than something you'd only catch
by toggling the focus variant one at a time. AC-3's only variant is
`landscape`, so switching to "By variant" on an AC-3 leg draws a single line
— the other nine simply have no points there. The table below can still show
several variants' rows at once via the checkboxes regardless of which chart
mode is active, so you can compare e.g. `none` against `all` without
switching the chart back and forth.

`develop` and `main` behave exactly as on [Quality trend](quality-trend.md):
separate tracks (main only advances on a release promotion), `develop`
collapsed to its latest commit by default to keep it from crowding `main`
out, and a 🏷 badge marking a row whose commit was tagged as a release.

**vs FFmpeg** / **vs DEE** are only populated on `landscape` rows — the
delta between this build's own `all`-tools E-AC-3 encode (or AC-3's
automatic-everything encode) and the corresponding tool's number in the
checked-in [external baseline](https://github.com/iainchesworthlabs/ac3forge/blob/main/tests/golden/external-baseline/manifest.json)
for that same leg, at the `baseline_version` recorded alongside it. A blank
cell on a `landscape` row means that leg's DEE score is still marked
unverified in the baseline manifest, not that the delta was zero.

## Where the data lives

Same mechanism as [Quality trend](quality-trend.md#where-the-data-lives): a
dedicated `quality-history` branch, `external-comparison-<branch>.jsonl`
this time, written by a job in `ci.yml` (`persist-external-comparison-trend`)
downstream of `ffmpeg-validate`'s compute-only `trend` step, on direct
pushes to `develop`/`main` only.
