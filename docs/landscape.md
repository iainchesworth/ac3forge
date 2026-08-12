# Landscape

How ac3forge's encoder compares to FFmpeg's and Dolby's own (DEE, the Dolby
Encoding Engine) at matched bitrates, release by release. This is the
headline number — see [Tool comparison trend](tool-comparison-trend.md) for
the commit-level, per-Annex-E-tool detail this page deliberately doesn't
show.

The comparison is necessarily one number per (leg, tool): neither FFmpeg's
nor DEE's own E-AC-3/AC-3 encoder exposes which internal coding tools it
used, so there's no apples-to-apples way to isolate "just coupling" or "just
spectral extension" against them the way this project can against its own
history. What's shown is `landscape` — this project's `all`-tools E-AC-3
encode (or AC-3's unconditionally-automatic encode) — since that's the
number a real user of either tool actually gets, not an internal detail.

<div id="landscape-app">
  <p class="landscape-status">Loading landscape data…</p>
</div>

<style>
#landscape-app { margin: 1.5em 0; }
.landscape-status { color: var(--md-default-fg-color--light); font-style: italic; }
.landscape-baseline { font-size: 0.85em; color: var(--md-default-fg-color--light); margin-bottom: 1em; }
.landscape-table-wrap { overflow-x: auto; }
#landscape-app table { width: 100%; border-collapse: collapse; font-size: 0.9em; }
#landscape-app th, #landscape-app td { padding: 0.4em 0.7em; text-align: left; border-bottom: 1px solid var(--md-default-fg-color--lightest); white-space: nowrap; }
.landscape-release { text-decoration: none; font-weight: 600; }
.landscape-release:hover { text-decoration: underline; }
.landscape-prerelease { font-weight: 400; font-style: italic; color: var(--md-default-fg-color--light); }
.landscape-delta-up { color: #2e7d32; font-weight: 600; }
.landscape-delta-down { color: #c62828; font-weight: 600; }
.landscape-na { color: var(--md-default-fg-color--light); font-style: italic; }
</style>

<script>
(function () {
  const REPO = "iainchesworth/ac3forge";
  const HISTORY_BRANCH = "quality-history";
  // Releases only ever happen on main (releasing.md's own promotion flow),
  // so unlike quality-trend.md/tool-comparison-trend.md there is only one
  // track to fetch here, not develop+main.
  const BRANCH = "main";

  const root = document.getElementById("landscape-app");

  function rawUrl(branch, file) {
    return `https://raw.githubusercontent.com/${REPO}/${branch}/${file}`;
  }

  function parseJsonl(text) {
    return text.split("\n").filter((l) => l.trim().length > 0).map((l) => JSON.parse(l));
  }

  async function fetchTrack() {
    try {
      const resp = await fetch(rawUrl(HISTORY_BRANCH, `external-comparison-${BRANCH}.jsonl`));
      if (!resp.ok) return [];
      return parseJsonl(await resp.text());
    } catch (e) {
      return [];
    }
  }

  async function fetchManifest() {
    try {
      const resp = await fetch(rawUrl(BRANCH, "tests/golden/external-baseline/manifest.json"));
      if (!resp.ok) return null;
      return await resp.json();
    } catch (e) {
      return null;
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
            shaMap[sha].date = rel.published_at || null;
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

  function deltaCell(value) {
    if (value === undefined || value === null) return '<span class="landscape-na">n/a</span>';
    const cls = value >= 0 ? "landscape-delta-up" : "landscape-delta-down";
    return `<span class="${cls}">${value >= 0 ? "+" : ""}${value.toFixed(2)} dB</span>`;
  }

  function buildBaselineInfo(manifest) {
    if (!manifest) {
      return '<p class="landscape-baseline">Current baseline metadata unavailable (manifest fetch failed or rate-limited).</p>';
    }
    return `<p class="landscape-baseline">Current baseline (v${manifest.baseline_version}, generated ${manifest.generated_date}):
      FFmpeg <code>${manifest.tools.ffmpeg.version}</code> ·
      DEE <code>${manifest.tools.dee.version}</code> —
      regenerated locally and reviewed by hand, see
      <a href="https://github.com/${REPO}/blob/main/tools/gen_external_baseline.py">tools/gen_external_baseline.py</a>.</p>`;
  }

  function buildTable(records, releasesBySha) {
    // Only landscape rows, and only commits carrying a release tag - this
    // page's entire reason for existing is the release-over-release view,
    // not a running commit-by-commit line (that's tool-comparison-trend.md).
    const rows = records.filter((r) => r.variant === "landscape" && releasesBySha[r.commit]);
    if (rows.length === 0) {
      return '<p class="landscape-status">No tagged-release rows yet - this fills in as releases are cut on main after this page landed.</p>';
    }
    const trs = rows
      .slice()
      .sort((a, b) => {
        const ra = releasesBySha[a.commit], rb = releasesBySha[b.commit];
        const da = ra.date || a.commit_date, db = rb.date || b.commit_date;
        if (da !== db) return db.localeCompare(da);
        return a.leg.localeCompare(b.leg);
      })
      .map((r) => {
        const release = releasesBySha[r.commit];
        return `<tr>
          <td><a class="landscape-release" href="${release.url}">🏷 ${release.name}</a>${release.prerelease ? ' <span class="landscape-prerelease">(prerelease)</span>' : ""}</td>
          <td>${(release.date || r.commit_date).slice(0, 10)}</td>
          <td><a href="${commitUrl(r.commit)}">${shortSha(r.commit)}</a></td>
          <td>${r.leg}</td>
          <td>${r.snr_db.toFixed(2)} dB</td>
          <td>${deltaCell(r.vs_ffmpeg_snr_db)}</td>
          <td>${deltaCell(r.vs_dee_snr_db)}</td>
          <td>${r.lsd_db === null ? "-" : r.lsd_db.toFixed(2) + " dB"}</td>
          <td>${r.baseline_version !== undefined ? "v" + r.baseline_version : "-"}</td>
        </tr>`;
      })
      .join("");
    return `<div class="landscape-table-wrap"><table>
      <thead><tr><th>Release</th><th>Date</th><th>Commit</th><th>Leg</th><th>ac3forge SNR</th><th>vs FFmpeg</th><th>vs DEE</th><th>LSD</th><th>Baseline</th></tr></thead>
      <tbody>${trs}</tbody>
    </table></div>`;
  }

  Promise.all([fetchTrack(), fetchReleaseShaMap(), fetchManifest()]).then(([records, releasesBySha, manifest]) => {
    root.innerHTML = `
      ${buildBaselineInfo(manifest)}
      ${buildTable(records, releasesBySha)}
    `;
  });
})();
</script>

## Reading it

Each row is one (tagged release, leg) result — a release cuts one commit on
`main`, and that commit contributes up to three rows (the AC-3 5.1, E-AC-3
stereo, and E-AC-3 5.1 legs). **vs FFmpeg** / **vs DEE** are the delta
between ac3forge's own SNR and that tool's number for the same leg at the
baseline version shown — positive (green) means ac3forge scored higher.
**n/a** on a `vs DEE` cell means that leg's DEE score is still marked
unverified in the baseline manifest (see that file's own header for why),
not that the comparison came out even.

The baseline itself — FFmpeg's and DEE's actual encoded output — is
regenerated locally, occasionally, and reviewed by hand as a normal PR (see
`tools/gen_external_baseline.py`'s own docstring); it is never re-run
automatically, and never runs in CI. The **Baseline** column names which
version of it a given release's numbers were compared against, so a jump in
that column marks where the external side of the comparison changed, not
ac3forge's own encoder.

## Where the data lives

Same `quality-history` branch mechanism as
[Quality trend](quality-trend.md#where-the-data-lives) and
[Tool comparison trend](tool-comparison-trend.md#where-the-data-lives) -
this page reads `external-comparison-main.jsonl` specifically (releases only
ever happen on `main`) and filters it down to commits the GitHub API reports
as tagged, joined client-side the same way quality-trend.md's own release
badges are.
