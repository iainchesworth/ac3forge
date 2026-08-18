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
#landscape-app th { vertical-align: bottom; }
/* One rule per metric group, so the three blocks read as blocks rather than
   as nine loose columns. */
.landscape-group-start { border-left: 1px solid var(--md-default-fg-color--lighter); }
.landscape-release { text-decoration: none; font-weight: 600; }
.landscape-release:hover { text-decoration: underline; }
.landscape-prerelease { font-weight: 400; font-style: italic; color: var(--md-default-fg-color--light); }
.landscape-delta-up { color: #2e7d32; font-weight: 600; }
.landscape-delta-down { color: #c62828; font-weight: 600; }
.landscape-na { color: var(--md-default-fg-color--light); font-style: italic; }
/* Alternated per release (not per row) so a release's 1-3 leg rows read as
   one band, and skimming down the table shows release boundaries at a
   glance rather than a flat wall of same-looking rows. */
.landscape-stripe-b { background: color-mix(in srgb, var(--md-default-fg-color) 6%, transparent); }
</style>

<script>
(function () {
  const REPO = "iainchesworthlabs/ac3forge";
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

  // Every vs_* value in the history file is ours-minus-theirs. For SNR and
  // MOS that means higher is better; for LSD - a distance - it is the other
  // way round, so the colour is chosen here rather than baked into the sign.
  function deltaCell(value, opts) {
    const { lowerIsBetter = false, unit = " dB", digits = 2 } = opts || {};
    if (value === undefined || value === null) return '<span class="landscape-na">n/a</span>';
    const better = lowerIsBetter ? value <= 0 : value >= 0;
    const cls = better ? "landscape-delta-up" : "landscape-delta-down";
    return `<span class="${cls}">${value >= 0 ? "+" : ""}${value.toFixed(digits)}${unit}</span>`;
  }

  function valueCell(value, unit, digits) {
    if (value === undefined || value === null) return '<span class="landscape-na">-</span>';
    return `${value.toFixed(digits)}${unit}`;
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
    let prevTag = null;
    let groupIndex = -1;
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
        if (release.tag !== prevTag) {
          groupIndex++;
          prevTag = release.tag;
        }
        const stripeCls = groupIndex % 2 === 1 ? " class=\"landscape-stripe-b\"" : "";
        return `<tr${stripeCls}>
          <td><a class="landscape-release" href="${release.url}">🏷 ${release.name}</a>${release.prerelease ? ' <span class="landscape-prerelease">(prerelease)</span>' : ""}</td>
          <td>${(release.date || r.commit_date).slice(0, 10)}</td>
          <td><a href="${commitUrl(r.commit)}">${shortSha(r.commit)}</a></td>
          <td>${r.leg}</td>
          <td class="landscape-group-start">${valueCell(r.snr_db, " dB", 2)}</td>
          <td>${deltaCell(r.vs_ffmpeg_snr_db)}</td>
          <td>${deltaCell(r.vs_dee_snr_db)}</td>
          <td class="landscape-group-start">${valueCell(r.lsd_db, " dB", 2)}</td>
          <td>${deltaCell(r.vs_ffmpeg_lsd_db, { lowerIsBetter: true })}</td>
          <td>${deltaCell(r.vs_dee_lsd_db, { lowerIsBetter: true })}</td>
          <td class="landscape-group-start">${valueCell(r.mos_lqo, "", 2)}</td>
          <td>${deltaCell(r.vs_ffmpeg_mos_lqo, { unit: "" })}</td>
          <td>${deltaCell(r.vs_dee_mos_lqo, { unit: "" })}</td>
          <td class="landscape-group-start">${r.baseline_version !== undefined ? "v" + r.baseline_version : "-"}</td>
        </tr>`;
      })
      .join("");
    return `<div class="landscape-table-wrap"><table>
      <thead>
        <tr>
          <th rowspan="2">Release</th><th rowspan="2">Date</th><th rowspan="2">Commit</th><th rowspan="2">Leg</th>
          <th colspan="3" class="landscape-group-start">SNR — waveform (higher better)</th>
          <th colspan="3" class="landscape-group-start">LSD — envelope (lower better)</th>
          <th colspan="3" class="landscape-group-start">MOS — perceptual (higher better)</th>
          <th rowspan="2" class="landscape-group-start">Baseline</th>
        </tr>
        <tr>
          <th class="landscape-group-start">ac3forge</th><th>vs FFmpeg</th><th>vs DEE</th>
          <th class="landscape-group-start">ac3forge</th><th>vs FFmpeg</th><th>vs DEE</th>
          <th class="landscape-group-start">ac3forge</th><th>vs FFmpeg</th><th>vs DEE</th>
        </tr>
      </thead>
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

## Spectrograms

A visual supplement to the table above — one image per leg, each stacking
the original source against ac3forge's own decode and, where the baseline
has a trustworthy score for it (see **n/a** below), FFmpeg's and DEE's own
decodes of the same material at the same bitrate.

<div class="landscape-spectrograms">
  <figure>
    <img src="https://raw.githubusercontent.com/iainchesworthlabs/ac3forge/quality-history/spectrograms/ac3-51-448.png" alt="ac3-51-448 spectrogram: original vs ac3forge vs FFmpeg" loading="lazy">
    <figcaption>ac3-51-448 (AC-3, 5.1 @ 448 kbit/s) — no DEE row: DEE's own decode of discrete 5.1 input drops a channel on the current baseline build, see the table's own <strong>n/a</strong> note.</figcaption>
  </figure>
  <figure>
    <img src="https://raw.githubusercontent.com/iainchesworthlabs/ac3forge/quality-history/spectrograms/eac3-stereo-192.png" alt="eac3-stereo-192 spectrogram: original vs ac3forge vs FFmpeg vs DEE" loading="lazy">
    <figcaption>eac3-stereo-192 (E-AC-3, stereo @ 192 kbit/s)</figcaption>
  </figure>
  <figure>
    <img src="https://raw.githubusercontent.com/iainchesworthlabs/ac3forge/quality-history/spectrograms/eac3-51-256.png" alt="eac3-51-256 spectrogram: original vs ac3forge vs FFmpeg" loading="lazy">
    <figcaption>eac3-51-256 (E-AC-3, 5.1 @ 256 kbit/s) — no DEE row, same reason as ac3-51-448.</figcaption>
  </figure>
</div>

<style>
.landscape-spectrograms { display: flex; flex-direction: column; gap: 1.5em; margin: 1.5em 0; }
.landscape-spectrograms figure { margin: 0; }
.landscape-spectrograms img { width: 100%; height: auto; border: 1px solid var(--md-default-fg-color--lightest); border-radius: 0.2em; }
.landscape-spectrograms figcaption { font-size: 0.85em; color: var(--md-default-fg-color--light); margin-top: 0.4em; }
</style>

These are **not** tied to any specific release row above — there is only a
single "latest" image per leg, regenerated on every push to `main` (i.e.
every release promotion, same cadence as a row landing in the table), never
one per historical release. If the image looks newer than the table row
you're comparing it against, it is: the images have no history of their own,
only a current snapshot. They come from the same `quality-history` branch
mechanism as the table's own numbers (see "Where the data lives" below) —
generated in CI by `tools/quality_race.py`'s `render_spectrograms()`
(`trend --spectrogram-dir`), decoding this build's own encode plus the
committed `tests/golden/external-baseline/` FFmpeg/DEE bitstreams — never
invoking FFmpeg's or DEE's own encoder, same boundary as the numbers.

## Reading it

Each row is one (tagged release, leg) result — a release cuts one commit on
`main`, and that commit contributes up to three rows (the AC-3 5.1, E-AC-3
stereo, and E-AC-3 5.1 legs). Alternating row shading marks where one
release's rows end and the next begins, so a release's leg rows read as one
band rather than blending into the wall of numbers below.

Three metrics are shown side by side, each with its own **vs FFmpeg** /
**vs DEE** delta against that tool's number for the same leg at the baseline
version shown. Green always means ac3forge came out better, which is a
*higher* number for SNR and MOS and a *lower* one for LSD — the stored
deltas are all plainly ours-minus-theirs, and only the colouring knows which
way each metric points.

No one of the three is the headline, and that is deliberate. E-AC-3's
coupling and spectral extension trade waveform fidelity for banded envelope
fidelity **on purpose** — that is what they are for — so waveform SNR alone
reports a working tool as a straight loss, while LSD alone rewards one that
has thrown the waveform away. Reading all three together is the only way the
comparison says something true about the encoder rather than about the
metric. (The per-tool detail behind that trade is in
[Tool comparison trend](tool-comparison-trend.md).)

**n/a** on a `vs DEE` cell means that leg's DEE score is still marked
unverified in the baseline manifest (see that file's own header for why),
not that the comparison came out even. **-** in an ac3forge cell means the
metric was not scored for that row at all: LSD is a measure of what the
Annex E tools trade away, so it is scored on the E-AC-3 legs only and the
AC-3 row leaves it blank, while MOS — a general quality prediction — is
scored on all three legs but is absent from any run whose environment lacked
`visqol-python`. A delta is shown only where both sides
have a real number, so a baseline generated before
`gen_external_baseline.py` grew its MOS column leaves those cells **n/a**
even where ac3forge's own MOS is present.

The baseline itself — FFmpeg's and DEE's actual encoded output — is
regenerated locally, occasionally, and reviewed by hand as a normal PR (see
`tools/gen_external_baseline.py`'s own docstring); it is never re-run
automatically, and never runs in CI. The **Baseline** column names which
version of it a given release's numbers were compared against, so a jump in
that column marks where the external side of the comparison changed, not
ac3forge's own encoder.

**MOS** is [ViSQOL](https://github.com/google/visqol)'s MOS-LQO (Mean
Opinion Score - Listening Quality Objective) in audio mode, a perceptual-
quality prediction from 1 (bad) to a ceiling around 4.75 — see [Tool
comparison trend](tool-comparison-trend.md#reading-it) for why ViSQOL over
PEAQ. It shows `-` on a release whose CI run didn't have `visqol-python`
installed — same graceful-degradation contract every other use of it in this
project follows, not a real zero — and its `vs` cells need the baseline side
too, so they stay **n/a** until a baseline version generated by a
`gen_external_baseline.py` run that had ViSQOL available lands. Of the three
metrics it is the only one that tries to answer "which sounds better", which
is why it is worth having beside the two that answer narrower questions
exactly.

## Where the data lives

Same `quality-history` branch mechanism as
[Quality trend](quality-trend.md#where-the-data-lives) and
[Tool comparison trend](tool-comparison-trend.md#where-the-data-lives) -
this page reads `external-comparison-main.jsonl` specifically (releases only
ever happen on `main`) and filters it down to commits the GitHub API reports
as tagged, joined client-side the same way quality-trend.md's own release
badges are.
