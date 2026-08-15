# Quality trend

Every push to `develop` or `main` that gets through the [gold-reference
gate](https://github.com/iainchesworth/ac3forge/blob/main/scripts/verify-gold-reference.sh)
(encode the checked-in golden 5.1 WAV, strict-decode with FFmpeg and with
`ac3cli`'s own decoder, delay-compensated SNR between the two) has its
per-channel numbers appended to history instead of only living in that run's
CI log. It turns the gate's own FFmpeg-oracle SNR check into a lightweight,
trended perceptual signal — the gate itself has run on every commit since it
landed; what's below is what makes the *numbers*, not just the pass/fail,
outlive the run that produced them.

The gate's own threshold (currently 55 dB, see `MIN_SNR_DB` in
`verify-gold-reference.sh`) is a fixed floor that always fails CI outright.
On top of that, the append step applies two trailing-baseline checks against
each run's own leg/codec history: a soft one (0.5 dB below the trailing
10-run mean) that only annotates a row below, never fails anything, and a
hard one (10 dB below that mean) that *does* fail the run — after the
numbers are still recorded here, so a big regression is never silently
un-recorded just because it also failed. See `REGRESSION_DROP_DB` and
`HARD_REGRESSION_DROP_DB` in `scripts/append-quality-history.py`.

<div id="quality-trend-app">
  <p class="quality-trend-status">Loading trend data…</p>
</div>

<style>
#quality-trend-app { margin: 1.5em 0; }
.quality-trend-status { color: var(--md-default-fg-color--light); font-style: italic; }
.quality-trend-controls { display: flex; gap: 1.25em; align-items: center; margin-bottom: 0.75em; flex-wrap: wrap; }
.quality-trend-controls label { font-size: 0.85em; color: var(--md-default-fg-color--light); display: inline-flex; align-items: center; gap: 0.35em; white-space: nowrap; }
.quality-trend-controls select {
  font: inherit; padding: 0.2em 0.5em; border-radius: 0.2em;
  border: 1px solid var(--md-default-fg-color--lightest);
  background: var(--md-default-bg-color); color: var(--md-default-fg-color);
}
.quality-trend-chart-wrap { overflow-x: auto; margin-bottom: 1em; }
.quality-trend-chart { display: block; }
.quality-trend-legend { display: flex; gap: 1.5em; font-size: 0.85em; margin: 0.5em 0 1em; flex-wrap: wrap; }
.quality-trend-legend span { display: inline-flex; align-items: center; gap: 0.4em; }
.quality-trend-legend i { width: 0.9em; height: 0.9em; border-radius: 50%; display: inline-block; }
.quality-trend-table-wrap { overflow-x: auto; }
#quality-trend-app table { width: 100%; border-collapse: collapse; font-size: 0.85em; }
#quality-trend-app th, #quality-trend-app td { padding: 0.35em 0.6em; text-align: left; border-bottom: 1px solid var(--md-default-fg-color--lightest); white-space: nowrap; }
.quality-trend-regression { color: var(--md-typeset-mark-color, #c62828); font-weight: 600; }
.quality-trend-secondary-check { color: var(--md-default-fg-color--light); cursor: help; }
.quality-trend-release-row { background: color-mix(in srgb, var(--md-accent-fg-color, #7c4dff) 8%, transparent); }
.quality-trend-release { text-decoration: none; font-weight: 600; }
.quality-trend-release:hover { text-decoration: underline; }
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
  // same judgment call, not a second source of truth for it. Only the soft
  // (non-failing) tier is displayed here - a hard regression fails its own CI
  // run directly (see _build.yml), so it doesn't need a table annotation to
  // be noticed too.
  const REGRESSION_WINDOW = 10;
  const REGRESSION_DROP_DB = 0.5;
  const TABLE_ROWS = 40;
  // WAV channel order ac3::io::ac3_layout_for(6) expects - see
  // tools/gen_gold_reference_wav.py - and so the order compare_wav.py's
  // channels_db is written in. Only meaningful for the current 6-channel 5.1
  // golden reference; anything else (e.g. a future Atmos-bed layout with a
  // different channel count) falls back to a plain index label rather than
  // guessing at a mapping.
  const CHANNEL_LABELS_51 = ["L", "R", "C", "LFE", "Ls", "Rs"];
  // Mirrors _build.yml's gold_reference matrix - the same 5 legs
  // scripts/append-quality-history.py strips the "gold-reference-" artifact
  // prefix down to. Fixed order/colors so a leg's line keeps the same color
  // across renders instead of shuffling with whichever legs happen to have
  // data in the current view.
  const LEGS = [
    { leg: "windows-msvc", color: "#7c4dff" },
    { leg: "windows-llvm", color: "#00acc1" },
    { leg: "linux-gcc", color: "#43a047" },
    { leg: "linux-llvm", color: "#fb8c00" },
    { leg: "macos-llvm", color: "#e53935" },
  ];

  const root = document.getElementById("quality-trend-app");

  // Filter/display state, mutated by the controls and re-read on every
  // render() - kept outside render() so a control change doesn't need to
  // thread its way back in as a parameter.
  const state = {
    codec: "ac3",
    // "branch": one line per branch, worst-of-5-legs per commit (the
    // original view - good for "did anything regress"). "leg": one line per
    // CI leg for a single branch, un-folded (good for "is one platform
    // drifting relative to the others over time") - see LEGS above. Never
    // both branch and leg as line dimensions at once: up to 5 legs x 2
    // branches = 10 lines was exactly the "unreadable" case worstPerCommit
    // was written to avoid, so leg view picks one branch instead of folding
    // it away.
    view: "branch",
    branches: { main: true, develop: true },
    // develop pushes far more often than main (every commit vs. only release
    // promotions) and fans out to the same 5 legs x 2 codecs per commit, so
    // showing its full history by default crowds main's rows out of the
    // table entirely. Collapsed to its latest commit until asked to expand.
    developFullHistory: false,
    // Which branch's per-leg lines are drawn in "leg" view. develop by
    // default since it has enough push frequency for the per-leg lines to
    // actually look like trends; main's own history is comparatively sparse.
    legBranch: "develop",
  };

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

  // Maps a commit SHA to the release tagged at it, keyed off the GitHub
  // REST API rather than anything in quality-history itself - release.yml
  // tags an existing main commit after the fact, so the tag never appears in
  // the quality-history record for that commit. Client-side and
  // best-effort, same as fetchTrack above: a rate-limited or offline
  // response just means no release markers, not a broken page.
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

  function channelLabel(idx, total) {
    return total === CHANNEL_LABELS_51.length ? CHANNEL_LABELS_51[idx] : `ch${idx}`;
  }

  function worstChannelLabel(r) {
    return channelLabel(r.channels_db.indexOf(r.worst_db), r.channels_db.length);
  }

  function channelBreakdownText(r) {
    return r.channels_db.map((v, i) => `${channelLabel(i, r.channels_db.length)} ${v.toFixed(2)} dB`).join(" · ");
  }

  function mostRecentCommit(records) {
    if (records.length === 0) return null;
    return records.reduce((a, b) => (a.commit_date > b.commit_date ? a : b)).commit;
  }

  // The records currently in scope given the branch checkboxes and the
  // develop-history collapse - both the chart and the table are built from
  // this, not from the raw fetch, so they always agree with what the
  // controls say should be visible.
  function visibleRecords(allRecords) {
    const latestDevelop = mostRecentCommit(allRecords.filter((r) => r.branch === "develop"));
    return allRecords.filter((r) => {
      if (!state.branches[r.branch]) return false;
      if (r.branch === "develop" && !state.developFullHistory) {
        return r.commit === latestDevelop;
      }
      return true;
    });
  }

  // verify-gold-reference.sh runs more than one check per codec now - e.g.
  // eac3's tools=none baseline, its tools=cpl variant, and the unrelated
  // eac3_cplbndstrce0 third-party-bitstream interop fixture all share
  // codec:"eac3" but compare fundamentally different things at deliberately
  // different SNR floors (see scripts/append-quality-history.py's own
  // "check" field, added for the same reason). This page's chart has only
  // ever shown one line per codec, so it sticks to each codec's original,
  // continuous baseline check (check === codec) rather than folding in the
  // newer variants - a record with no "check" at all is older history
  // written before that field existed, and was always that baseline check
  // by construction, so it counts too.
  function isPrimaryCheck(r) {
    return !r.check || r.check === r.codec;
  }

  // One point per commit per branch: the worst worst_db across every leg for
  // the selected codec, since a per-leg-and-codec chart (up to 5 legs x 2
  // codecs x 2 branches) would be unreadable as lines. Leg-level detail is
  // still in the table below, just not the chart.
  function worstPerCommit(records, codec) {
    const byCommit = new Map();
    for (const r of records) {
      if (r.codec !== codec || !isPrimaryCheck(r)) continue;
      const cur = byCommit.get(r.commit);
      if (!cur || r.worst_db < cur.worst_db) {
        byCommit.set(r.commit, r);
      }
    }
    return Array.from(byCommit.values()).sort((a, b) => a.commit_date.localeCompare(b.commit_date));
  }

  // The un-folded counterpart to worstPerCommit above: every (leg, codec,
  // branch) record, one series per leg, nothing dropped. This is what "leg"
  // view charts - the same per-leg detail worstPerCommit discards, just kept
  // instead of collapsed to a single worst-of-legs point.
  function perLegSeries(records, codec, branch) {
    const byLeg = {};
    for (const legDef of LEGS) byLeg[legDef.leg] = [];
    for (const r of records) {
      if (r.codec !== codec || r.branch !== branch || !isPrimaryCheck(r)) continue;
      if (byLeg[r.leg]) byLeg[r.leg].push(r);
    }
    for (const leg of Object.keys(byLeg)) {
      byLeg[leg].sort((a, b) => a.commit_date.localeCompare(b.commit_date));
    }
    return byLeg;
  }

  // Always computed against the full, unfiltered history for the (leg,
  // codec, check, branch) - collapsing develop's *display* to its latest
  // commit shouldn't change what counts as a regression against its own
  // trailing average. "check" (falling back to codec for pre-"check" history,
  // same reasoning as isPrimaryCheck above) keeps this matched to the same
  // check the row itself came from, instead of averaging across unrelated
  // checks that happen to share a codec.
  function regressionBaseline(allRecords, leg, codec, check, branch, beforeCommitDate) {
    const trail = allRecords
      .filter((r) => r.leg === leg && r.codec === codec && (r.check || r.codec) === check &&
                     r.branch === branch && r.commit_date < beforeCommitDate)
      .sort((a, b) => a.commit_date.localeCompare(b.commit_date))
      .slice(-REGRESSION_WINDOW);
    if (trail.length === 0) return null;
    return trail.reduce((sum, r) => sum + r.worst_db, 0) / trail.length;
  }

  // Shared, calendar-time x-axis (not a per-track point index) - required
  // once tracks can carry very different point counts, e.g. develop's full
  // history against main's single latest entry: an index-based axis would
  // stack main's one point at the start of the line instead of at its actual
  // (recent) date.
  //
  // Generic over what a "track" is: branch view passes one track per branch
  // (worst-of-legs points), leg view passes one track per CI leg for a
  // single branch (un-folded points) - same x/y math and SVG either way,
  // only the line count and labels differ. Each track is
  // { key, label, color, points }.
  function buildChart(tracks, codec, releasesBySha) {
    const width = 760, height = 220, pad = { top: 12, right: 12, bottom: 32, left: 42 };
    const allPoints = tracks.flatMap((t) => t.points);
    if (allPoints.length === 0) {
      return `<p class="quality-trend-status">No ${codec} history in the current view.</p>`;
    }
    const dbValues = allPoints.map((p) => p.worst_db);
    const minDb = Math.min(...dbValues, 20);
    const maxDb = Math.max(...dbValues) + 2;
    const times = allPoints.map((p) => Date.parse(p.commit_date));
    const minT = Math.min(...times);
    const maxT = Math.max(...times);

    const x = (t) => pad.left + (maxT === minT ? (width - pad.left - pad.right) / 2 : ((t - minT) / (maxT - minT)) * (width - pad.left - pad.right));
    const y = (db) => height - pad.bottom - ((db - minDb) / (maxDb - minDb)) * (height - pad.top - pad.bottom);

    let svg = `<svg class="quality-trend-chart" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}" role="img" aria-label="Worst-channel SNR by commit date, ${codec}">`;
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
        const path = pts.map((p, i) => `${i === 0 ? "M" : "L"}${x(Date.parse(p.commit_date)).toFixed(1)},${y(p.worst_db).toFixed(1)}`).join(" ");
        svg += `<path d="${path}" fill="none" stroke="${track.color}" stroke-width="2"/>`;
      }
      pts.forEach((p) => {
        const cx = x(Date.parse(p.commit_date)).toFixed(1);
        const cy = y(p.worst_db).toFixed(1);
        const release = releasesBySha[p.commit];
        const title = `${track.label} ${shortSha(p.commit)} - ${p.worst_db.toFixed(2)} dB (${worstChannelLabel(p)}, worst of ${channelBreakdownText(p)}) on ${p.commit_date.slice(0, 10)}${release ? ` - release ${release.name}` : ""}`;
        svg += `<circle cx="${cx}" cy="${cy}" r="3" fill="${track.color}"><title>${title}</title></circle>`;
        if (release) {
          svg += `<circle cx="${cx}" cy="${cy}" r="6.5" fill="none" stroke="${track.color}" stroke-width="1.5" stroke-dasharray="2,1.5"><title>${title}</title></circle>`;
        }
      });
    }
    svg += "</svg>";
    return svg;
  }

  function buildLegend(tracks, releasesBySha) {
    const items = tracks.map((t) => `<span><i style="background:${t.color}"></i>${t.label}</span>`);
    const anyRelease = tracks.some((t) => t.points.some((p) => releasesBySha[p.commit]));
    if (anyRelease) {
      items.push('<span><i style="background:none;border:1.5px dashed var(--md-default-fg-color--light);"></i>tagged release</span>');
    }
    return `<div class="quality-trend-legend">${items.join("")}</div>`;
  }

  // Every check for the row's codec, in a single dedicated column, so two
  // checks that share a codec (and can carry very different SNR floors -
  // see isPrimaryCheck above) are never left to be told apart only by
  // eyeballing the bitrate or the worst-dB number. checkLabel falls back to
  // the codec name for pre-"check" history, same reasoning as isPrimaryCheck.
  function checkLabel(r) {
    return r.check || r.codec;
  }

  function buildTable(rows, allRecords, releasesBySha) {
    const trs = rows
      .slice()
      .sort((a, b) => b.commit_date.localeCompare(a.commit_date))
      .slice(0, TABLE_ROWS)
      .map((r) => {
        const baseline = regressionBaseline(allRecords, r.leg, r.codec, r.check || r.codec, r.branch, r.commit_date);
        const regressed = baseline !== null && baseline - r.worst_db >= REGRESSION_DROP_DB;
        const flag = regressed
          ? `<span class="quality-trend-regression" title="${(baseline - r.worst_db).toFixed(2)} dB below the trailing ${REGRESSION_WINDOW}-run mean (${baseline.toFixed(2)} dB)">▼ regression</span>`
          : "";
        const release = releasesBySha[r.commit];
        const releaseBadge = release
          ? `<a class="quality-trend-release" href="${release.url}" title="${release.prerelease ? "Prerelease" : "Release"} tagged at this commit">🏷 ${release.name}</a>`
          : "";
        const check = isPrimaryCheck(r)
          ? checkLabel(r)
          : `${checkLabel(r)} <span class="quality-trend-secondary-check" title="Not ${r.codec}'s primary round-trip check - a separate fixture with its own SNR floor and its own trailing baseline, not comparable to the ${r.codec} row above it">†</span>`;
        return `<tr${release ? ' class="quality-trend-release-row"' : ""}>
          <td>${r.commit_date.slice(0, 10)}</td>
          <td>${r.branch}</td>
          <td><a href="${commitUrl(r.commit)}">${shortSha(r.commit)}</a></td>
          <td>${r.leg}</td>
          <td>${r.codec} @ ${r.bitrate_kbps} kbps</td>
          <td>${check}</td>
          <td title="${channelBreakdownText(r)}">${worstChannelLabel(r)} ${r.worst_db.toFixed(2)} dB</td>
          <td>${releaseBadge}</td>
          <td>${flag}</td>
        </tr>`;
      })
      .join("");
    if (trs === "") {
      return '<p class="quality-trend-status">No rows in the current view - try a different branch/codec combination.</p>';
    }
    return `<div class="quality-trend-table-wrap"><table>
      <thead><tr><th>Date</th><th>Branch</th><th>Commit</th><th>Leg</th><th>Codec</th><th>Check</th><th>Worst channel</th><th>Release</th><th></th></tr></thead>
      <tbody>${trs}</tbody>
    </table></div>`;
  }

  function buildControls() {
    const legView = state.view === "leg";
    return `
      <div class="quality-trend-controls">
        <label for="quality-trend-codec">Codec
          <select id="quality-trend-codec">
            <option value="ac3" ${state.codec === "ac3" ? "selected" : ""}>AC-3</option>
            <option value="eac3" ${state.codec === "eac3" ? "selected" : ""}>E-AC-3</option>
          </select>
        </label>
        <label for="quality-trend-view">Chart
          <select id="quality-trend-view">
            <option value="branch" ${!legView ? "selected" : ""}>Worst of legs, by branch</option>
            <option value="leg" ${legView ? "selected" : ""}>By platform leg</option>
          </select>
        </label>
        ${legView ? `
          <label for="quality-trend-leg-branch">Branch
            <select id="quality-trend-leg-branch">
              <option value="develop" ${state.legBranch === "develop" ? "selected" : ""}>develop</option>
              <option value="main" ${state.legBranch === "main" ? "selected" : ""}>main</option>
            </select>
          </label>
        ` : `
          <label><input type="checkbox" id="quality-trend-branch-main" ${state.branches.main ? "checked" : ""}/> main</label>
          <label><input type="checkbox" id="quality-trend-branch-develop" ${state.branches.develop ? "checked" : ""}/> develop</label>
        `}
        ${!legView && state.branches.develop ? `<label><input type="checkbox" id="quality-trend-develop-history" ${state.developFullHistory ? "checked" : ""}/> develop: show full history</label>` : ""}
      </div>
    `;
  }

  function attachControlListeners(allRecords, releasesBySha) {
    document.getElementById("quality-trend-codec").addEventListener("change", (e) => {
      state.codec = e.target.value;
      render(allRecords, releasesBySha);
    });
    document.getElementById("quality-trend-view").addEventListener("change", (e) => {
      state.view = e.target.value;
      render(allRecords, releasesBySha);
    });
    const legBranchSelect = document.getElementById("quality-trend-leg-branch");
    if (legBranchSelect) {
      legBranchSelect.addEventListener("change", (e) => {
        state.legBranch = e.target.value;
        render(allRecords, releasesBySha);
      });
    }
    const mainToggle = document.getElementById("quality-trend-branch-main");
    if (mainToggle) {
      mainToggle.addEventListener("change", (e) => {
        state.branches.main = e.target.checked;
        render(allRecords, releasesBySha);
      });
    }
    const developToggle = document.getElementById("quality-trend-branch-develop");
    if (developToggle) {
      developToggle.addEventListener("change", (e) => {
        state.branches.develop = e.target.checked;
        render(allRecords, releasesBySha);
      });
    }
    const historyToggle = document.getElementById("quality-trend-develop-history");
    if (historyToggle) {
      historyToggle.addEventListener("change", (e) => {
        state.developFullHistory = e.target.checked;
        render(allRecords, releasesBySha);
      });
    }
  }

  // Builds this render's tracks per state.view - branch view folds each
  // branch down to its worst-of-legs line (worstPerCommit); leg view
  // un-folds a single branch into one line per CI leg (perLegSeries). Kept
  // out of render() itself so render() stays about assembling the page, not
  // about which view is active.
  function buildTracks(visible) {
    if (state.view === "leg") {
      const byLeg = perLegSeries(visible, state.codec, state.legBranch);
      return LEGS.map((legDef) => ({
        key: legDef.leg,
        label: legDef.leg,
        color: legDef.color,
        points: byLeg[legDef.leg] || [],
      }));
    }
    return TRACKS.map((t) => ({
      key: t.branch,
      label: t.branch === "develop" && !state.developFullHistory ? `${t.branch} (latest commit only)` : t.branch,
      color: t.color,
      points: state.branches[t.branch] ? worstPerCommit(visible.filter((r) => r.branch === t.branch), state.codec) : [],
    }));
  }

  function render(allRecords, releasesBySha) {
    // Leg view always looks at one branch's full history - the whole point
    // is seeing a per-leg trend over many commits, so the develop-collapse
    // and other-branch filters that branch view uses don't apply here.
    const visible = state.view === "leg"
      ? allRecords.filter((r) => r.branch === state.legBranch)
      : visibleRecords(allRecords);
    const tracks = buildTracks(visible);
    // The table used to get the same `visible` list the chart starts from -
    // every codec and every check, unfiltered - while only the chart applied
    // state.codec and isPrimaryCheck. That let the table interleave rows the
    // Codec control claimed to be scoping (even a different codec entirely)
    // with no per-row way to tell them apart, which is exactly the
    // conflation isPrimaryCheck exists to prevent in the chart. Scoping the
    // table to the selected codec too - while still showing every check
    // within it, distinguished by the Check column above - keeps secondary
    // checks visible instead of hidden, but never unlabelled next to a
    // codec's primary series.
    const tableRows = visible.filter((r) => r.codec === state.codec);
    root.innerHTML = `
      ${buildControls()}
      <div class="quality-trend-chart-wrap">${buildChart(tracks, state.codec, releasesBySha)}</div>
      ${buildLegend(tracks, releasesBySha)}
      ${buildTable(tableRows, allRecords, releasesBySha)}
    `;
    attachControlListeners(allRecords, releasesBySha);
  }

  Promise.all([...TRACKS.map((t) => fetchTrack(t.branch)), fetchReleaseShaMap()]).then((results) => {
    const releasesBySha = results.pop();
    const allRecords = [];
    TRACKS.forEach((t, i) => allRecords.push(...results[i]));
    if (allRecords.length === 0) {
      root.innerHTML = '<p class="quality-trend-status">No quality-trend history yet - it is written by CI on the first push to develop or main after this page landed.</p>';
      return;
    }
    render(allRecords, releasesBySha);
  });
})();
</script>

## Reading it

Each row is one (commit, CI leg, codec) result — the gate runs on every
`gold_reference` leg (`windows-msvc`, `windows-llvm`, `linux-gcc`,
`linux-llvm`, `macos-llvm`; not the ASan+UBSan leg, which stays
diagnostic-only), so a single commit contributes up to five rows per codec.

The **Chart** control picks what the lines represent. "Worst of legs, by
branch" (the default) plots the worst of the five legs per commit, one line
per branch, against a shared calendar x-axis — a cross-leg floating-point
difference (a ~62 dB vs. ~68 dB macOS/Linux split is a known, expected
effect of platform floating-point differences) is expected and not itself a
regression, so folding it away is deliberate here: this view answers "did
*anything* regress," not "which platform." "By platform leg" answers that
second question instead — it un-folds a single branch (picked with the
**Branch** control that replaces the branch checkboxes in this view) into
one line per leg, so a leg drifting relative to the other four, or trending
down over many commits while the rest hold steady, is visible as a shape in
the chart rather than something you'd only catch by scanning the table leg
by leg. Both views read the same underlying rows; nothing about which view
is active changes what counts as a regression in the table below.

`develop` and `main` are shown as separate tracks because they represent
different points in the codec's history — `main` only advances on a release
promotion, so it should read as a strictly-behind, occasionally-jumping
version of `develop`'s line, not a second independent series. By default
`develop` is collapsed to just its latest commit (its push frequency and
five-leg fan-out would otherwise crowd `main`'s rows out of the table
entirely) — use the "develop: show full history" control to expand it, or
uncheck a branch's box to hide it from the chart and table.

The **Codec** control scopes the table as well as the chart — picking
`E-AC-3` shows only `eac3` rows, never an unrelated `ac3` row sorted in by
date alone. Within that codec, the table still shows every **Check**:
`verify-gold-reference.sh` can run more than one check per codec (e.g.
`eac3`'s own baseline round-trip alongside `eac3_cplbndstrce0`, a real
third-party FFmpeg bitstream used to regression-test an Annex E decode fix),
and those checks compare fundamentally different things at deliberately
different SNR floors — one is not a worse day for the other. A `†` marks
any check that isn't that codec's primary, continuous series (the one the
chart plots and the dropdown otherwise implies); hover it for why. Never
read two different Check values as one continuous line, even when they
share a Codec and a date range — a `†` row's own trailing baseline (used
for the regression flag below) is computed only against its own check
history, precisely so a steady 25 dB interop floor can't look like a crash
relative to a steady 68 dB round-trip series, or vice versa.

The **worst channel** column names which of the six channels was worst
(`L R C LFE Ls Rs`, the golden reference's WAV channel order), and hovering
it shows every channel's number. In practice it is almost always one of the
two surrounds (`Ls`/`Rs`) — 15-20 dB below the front channels, consistently,
across every leg and commit recorded so far — which tracks with the encoder
allocating fewer bits to the less-dominant surround channels, not a
per-run fluke.

A 🏷 badge marks a row whose commit was tagged as a GitHub release (fetched
client-side from the GitHub API, best-effort — it silently shows nothing if
that call is rate-limited or offline). Release tagging happens after the
fact, on an existing `main` commit, so the badge is a join against the
commit SHA already in quality-history, not a separate data source.

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
pass, so — unlike a from-scratch perceptual pass, which would need a nightly
cadence to bound cost — doing this on every push costs nothing extra to
compute; only a JSON append and a git push are new.

`main`'s history has a real gap before 2026-08-10: `ci.yml`'s concurrency
group used to key push runs on branch name alone with `cancel-in-progress`
on, so a burst of merges landing within a build's runtime cancelled every
run but the last, silently dropping the gold-reference gate — and this
page's append step with it — before either finished. Push runs are now keyed
per-commit so this can't happen going forward; the specific commits already
lost to it were backfilled by hand rather than left blank.
