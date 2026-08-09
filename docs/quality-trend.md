# Quality trend

Every push to `develop` or `main` that gets through the [gold-reference
gate](https://github.com/iainchesworth/ac3forge/blob/main/scripts/verify-gold-reference.sh)
(encode the checked-in golden 5.1 WAV, strict-decode with FFmpeg and with
`ac3cli`'s own decoder, delay-compensated SNR between the two) has its
per-channel numbers appended to history instead of only living in that run's
CI log. This is [research.md](project/research.md#7-validation-pyramid)'s L3
(FFmpeg oracle) plus a lightweight L4 (perceptual SNR, trend lines) — the gate
itself has run on every commit since it landed; what's below is what makes
the *numbers*, not just the pass/fail, outlive the run that produced them.

The gate's own threshold (currently 30 dB, see `MIN_SNR_DB` in
`verify-gold-reference.sh`) is a fixed floor and still the only thing that
fails CI. The regression flag below is a separate, softer signal — a run more
than 3 dB under its own trailing 10-run average, on the same leg and codec —
and it never fails a build; it only annotates one in the table.

<div id="quality-trend-app">
  <p class="quality-trend-status">Loading trend data…</p>
</div>

<style>
#quality-trend-app { margin: 1.5em 0; }
.quality-trend-status { color: var(--md-default-fg-color--light); font-style: italic; }
.quality-trend-controls { display: flex; gap: 1em; align-items: center; margin-bottom: 0.75em; flex-wrap: wrap; }
.quality-trend-controls label { font-size: 0.85em; color: var(--md-default-fg-color--light); }
.quality-trend-controls select {
  font: inherit; padding: 0.2em 0.5em; border-radius: 0.2em;
  border: 1px solid var(--md-default-fg-color--lightest);
  background: var(--md-default-bg-color); color: var(--md-default-fg-color);
}
.quality-trend-chart-wrap { overflow-x: auto; margin-bottom: 1em; }
.quality-trend-chart { display: block; }
.quality-trend-legend { display: flex; gap: 1.5em; font-size: 0.85em; margin: 0.5em 0 1em; }
.quality-trend-legend span { display: inline-flex; align-items: center; gap: 0.4em; }
.quality-trend-legend i { width: 0.9em; height: 0.9em; border-radius: 50%; display: inline-block; }
.quality-trend-table-wrap { overflow-x: auto; }
#quality-trend-app table { width: 100%; border-collapse: collapse; font-size: 0.85em; }
#quality-trend-app th, #quality-trend-app td { padding: 0.35em 0.6em; text-align: left; border-bottom: 1px solid var(--md-default-fg-color--lightest); }
.quality-trend-regression { color: var(--md-typeset-mark-color, #c62828); font-weight: 600; }
</style>

<script>
(function () {
  const REPO = "iainchesworth/ac3forge";
  const HISTORY_BRANCH = "quality-history";
  const TRACKS = [
    { branch: "develop", color: "#7c4dff" },
    { branch: "main", color: "#00acc1" },
  ];
  // Mirrors scripts/append-quality-history.py's own constants - keep these two in
  // sync if that script's thresholds change; this is a display-only echo of the
  // same judgment call, not a second source of truth for it.
  const REGRESSION_WINDOW = 10;
  const REGRESSION_DROP_DB = 3.0;
  const TABLE_ROWS = 40;

  const root = document.getElementById("quality-trend-app");

  function rawUrl(branch, file) {
    return `https://raw.githubusercontent.com/${REPO}/${branch}/${file}`;
  }

  function parseJsonl(text) {
    return text.split("\n").filter((l) => l.trim().length > 0).map((l) => JSON.parse(l));
  }

  async function fetchTrack(branch) {
    try {
      const resp = await fetch(rawUrl(HISTORY_BRANCH, `${branch}.jsonl`));
      if (!resp.ok) return [];
      return parseJsonl(await resp.text());
    } catch (e) {
      return [];
    }
  }

  function shortSha(sha) {
    return sha.slice(0, 8);
  }

  function commitUrl(sha) {
    return `https://github.com/${REPO}/commit/${sha}`;
  }

  // One point per commit per branch: the worst worst_db across every leg for
  // the selected codec, since a per-leg-and-codec chart (up to 5 legs x 2
  // codecs x 2 branches) would be unreadable as lines. Leg-level detail is
  // still in the table below, just not the chart.
  function worstPerCommit(records, codec) {
    const byCommit = new Map();
    for (const r of records) {
      if (r.codec !== codec) continue;
      const cur = byCommit.get(r.commit);
      if (!cur || r.worst_db < cur.worst_db) {
        byCommit.set(r.commit, r);
      }
    }
    return Array.from(byCommit.values()).sort((a, b) => a.commit_date.localeCompare(b.commit_date));
  }

  function regressionBaseline(records, leg, codec, branch, beforeCommitDate) {
    const trail = records
      .filter((r) => r.leg === leg && r.codec === codec && r.branch === branch && r.commit_date < beforeCommitDate)
      .sort((a, b) => a.commit_date.localeCompare(b.commit_date))
      .slice(-REGRESSION_WINDOW);
    if (trail.length === 0) return null;
    return trail.reduce((sum, r) => sum + r.worst_db, 0) / trail.length;
  }

  function buildChart(seriesByBranch, codec) {
    const width = 760, height = 220, pad = { top: 12, right: 12, bottom: 24, left: 42 };
    const allPoints = TRACKS.flatMap((t) => seriesByBranch[t.branch] || []);
    if (allPoints.length === 0) {
      return `<p class="quality-trend-status">No ${codec} history yet.</p>`;
    }
    const dbValues = allPoints.map((p) => p.worst_db);
    const minDb = Math.min(...dbValues, 20);
    const maxDb = Math.max(...dbValues) + 2;
    const maxLen = Math.max(1, ...TRACKS.map((t) => (seriesByBranch[t.branch] || []).length));

    const x = (i, len) => pad.left + (len <= 1 ? 0 : (i / (len - 1)) * (width - pad.left - pad.right));
    const y = (db) => height - pad.bottom - ((db - minDb) / (maxDb - minDb)) * (height - pad.top - pad.bottom);

    let svg = `<svg class="quality-trend-chart" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}" role="img" aria-label="Worst-channel SNR by commit, ${codec}">`;
    // Gridlines + axis labels, 4 bands.
    for (let i = 0; i <= 4; i++) {
      const db = minDb + ((maxDb - minDb) * i) / 4;
      const gy = y(db);
      svg += `<line x1="${pad.left}" y1="${gy}" x2="${width - pad.right}" y2="${gy}" stroke="var(--md-default-fg-color--lightest)" stroke-width="1"/>`;
      svg += `<text x="${pad.left - 6}" y="${gy + 3}" text-anchor="end" font-size="10" fill="var(--md-default-fg-color--light)">${db.toFixed(0)}</text>`;
    }
    for (const track of TRACKS) {
      const pts = seriesByBranch[track.branch] || [];
      if (pts.length === 0) continue;
      const path = pts.map((p, i) => `${i === 0 ? "M" : "L"}${x(i, pts.length).toFixed(1)},${y(p.worst_db).toFixed(1)}`).join(" ");
      svg += `<path d="${path}" fill="none" stroke="${track.color}" stroke-width="2"/>`;
      pts.forEach((p, i) => {
        svg += `<circle cx="${x(i, pts.length).toFixed(1)}" cy="${y(p.worst_db).toFixed(1)}" r="3" fill="${track.color}"><title>${track.branch} ${shortSha(p.commit)} - ${p.worst_db.toFixed(2)} dB (${p.commit_date})</title></circle>`;
      });
    }
    svg += "</svg>";
    return svg;
  }

  function buildLegend() {
    return `<div class="quality-trend-legend">${TRACKS.map(
      (t) => `<span><i style="background:${t.color}"></i>${t.branch}</span>`
    ).join("")}</div>`;
  }

  function buildTable(allRecords) {
    const rows = allRecords
      .slice()
      .sort((a, b) => b.commit_date.localeCompare(a.commit_date))
      .slice(0, TABLE_ROWS)
      .map((r) => {
        const baseline = regressionBaseline(allRecords, r.leg, r.codec, r.branch, r.commit_date);
        const regressed = baseline !== null && baseline - r.worst_db >= REGRESSION_DROP_DB;
        const flag = regressed
          ? `<span class="quality-trend-regression" title="${(baseline - r.worst_db).toFixed(2)} dB below the trailing ${REGRESSION_WINDOW}-run mean (${baseline.toFixed(2)} dB)">▼ regression</span>`
          : "";
        return `<tr>
          <td>${r.commit_date.slice(0, 10)}</td>
          <td>${r.branch}</td>
          <td><a href="${commitUrl(r.commit)}">${shortSha(r.commit)}</a></td>
          <td>${r.leg}</td>
          <td>${r.codec} @ ${r.bitrate_kbps} kbps</td>
          <td>${r.worst_db.toFixed(2)} dB</td>
          <td>${flag}</td>
        </tr>`;
      })
      .join("");
    return `<div class="quality-trend-table-wrap"><table>
      <thead><tr><th>Date</th><th>Branch</th><th>Commit</th><th>Leg</th><th>Codec</th><th>Worst channel</th><th></th></tr></thead>
      <tbody>${rows}</tbody>
    </table></div>`;
  }

  function render(allRecords, codec) {
    const seriesByBranch = {};
    for (const track of TRACKS) {
      seriesByBranch[track.branch] = worstPerCommit(
        allRecords.filter((r) => r.branch === track.branch),
        codec
      );
    }
    root.innerHTML = `
      <div class="quality-trend-controls">
        <label for="quality-trend-codec">Codec</label>
        <select id="quality-trend-codec">
          <option value="ac3" ${codec === "ac3" ? "selected" : ""}>AC-3 (448 kbps)</option>
          <option value="eac3" ${codec === "eac3" ? "selected" : ""}>E-AC-3 (256 kbps)</option>
        </select>
      </div>
      <div class="quality-trend-chart-wrap">${buildChart(seriesByBranch, codec)}</div>
      ${buildLegend()}
      ${buildTable(allRecords)}
    `;
    document.getElementById("quality-trend-codec").addEventListener("change", (e) => {
      render(allRecords, e.target.value);
    });
  }

  Promise.all(TRACKS.map((t) => fetchTrack(t.branch))).then((results) => {
    const allRecords = [];
    TRACKS.forEach((t, i) => allRecords.push(...results[i]));
    if (allRecords.length === 0) {
      root.innerHTML = '<p class="quality-trend-status">No quality-trend history yet - it is written by CI on the first push to develop or main after this page landed.</p>';
      return;
    }
    render(allRecords, "ac3");
  });
})();
</script>

## Reading it

Each row is one (commit, CI leg, codec) result — the gate runs on every
`gold_reference` leg (`windows-msvc`, `windows-llvm`, `linux-gcc`,
`linux-llvm`, `macos-llvm`; not the ASan+UBSan leg, which stays
diagnostic-only), so a single commit contributes up to five rows per codec.
The chart plots the worst of those per commit, per branch — a cross-leg
floating-point difference (see the ~62 dB vs. ~68 dB macOS/Linux split noted
in [research.md](project/research.md)) is expected and not itself a
regression.

`develop` and `main` are shown as separate tracks because they represent
different points in the codec's history — `main` only advances on a release
promotion, so it should read as a strictly-behind, occasionally-jumping
version of `develop`'s line, not a second independent series.

## Where the data lives

Results are appended to a dedicated `quality-history` branch (`develop.jsonl`
/ `main.jsonl`), not `gh-pages` — `mkdocs gh-deploy` replaces gh-pages'
entire tree on every deploy, which would silently discard anything appended
there outside of what `mkdocs build` itself generates. This page fetches the
two files directly from `raw.githubusercontent.com` client-side, so a new
`develop` push shows up here without waiting on a docs deploy (which,
per [docs.yml](https://github.com/iainchesworth/ac3forge/blob/main/.github/workflows/docs.yml),
only runs on push to `main`).

History is written by a job in `_build.yml` that runs after every
`gold_reference` leg passes, on direct pushes to `develop` or `main` only —
never on a pull request, so unmerged work never pollutes the trend. It reuses
numbers the gate already computed rather than re-running the encode/decode
pass, so — unlike [research.md](project/research.md#7-validation-pyramid)'s
L4 entry, which calls for a nightly cadence to bound cost — doing this on
every push costs nothing extra to compute; only a JSON append and a git push
are new.
