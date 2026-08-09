# Handoff: ac3forge GUI redesign — the two-pane workbench

## Overview

A redesign of the ac3forge desktop GUI, replacing the current nine-card vertical scroll column
(~1,950 px tall at its fullest) with a two-pane workbench: the **signal** on a permanent left rail,
the **stream** in a tabbed panel on the right, and a persistent plan line above and command bar
below. It answers the eleven open questions in `docs/DESIGN-BRIEF.md` §6 and absorbs five later
change prompts (dynamic channel counts, real object motion, a basic/advanced split, the live demo
scenario, and the channel-picker constraint model).

Target branch: `develop`. Source of truth for current behaviour: `src/gui/qml/Main.qml`,
`src/gui/encoder_controller.{hpp,cpp}`, `docs/DESIGN-BRIEF.md`.

## About the design files

`ac3forge Workbench.dc.html` in this bundle is a **design reference written in HTML** — a prototype
of intended layout, behaviour and copy. It is not production code and nothing in it should be copied
into the repo. The task is to **recreate it in the existing environment**: Qt Quick / QML with Qt
Quick Controls, pinned to the Fusion style, driven by the existing `EncoderController`. Every value
below is expressed so it can be written straight into QML.

Open the file in any browser. The bar along the bottom ("Prototype states") switches between the six
states; it is prototype scaffolding and must **not** be built.

## Fidelity

**High-fidelity.** Colours, type, spacing and states are final and should be matched. The one
deliberate open question is theme: the prototype is drawn in the light palette because the brief
(§5) requires light *and* dark, and light is the one that does not exist yet. The dark theme must be
derived from the same tokens (see *Design tokens → Theming*), not hand-tuned separately.

---

## The window

Minimum **1280 × 900**; below that the panes reflow (rail floors at 340 px, the Format grid wraps at
180 px columns) rather than clip. The brief's stated 720 × 560 minimum is no longer honestly
achievable with this content; either raise the minimum in `main.cpp` or accept horizontal scrolling
under 1280.

Top-level structure, top to bottom:

1. **Title bar** — 34 px, `--color-neutral-200` fill, 1 px bottom divider. Window title reads
   `ac3forge — <source>` where source is `orbit51.wav`, `live capture` or `no source`.
2. **Header** — 14 px / 20 px padding, **2 px** bottom divider. Left: wordmark `ac3forge` (Archivo
   800, 22 px, letter-spacing −0.01em) + subtitle (12 px, neutral-700)
   "Clean-room AC-3 / E-AC-3 encoder — ATSC A/52, ETSI TS 103 420". Right: a `Controls` segmented
   control (Basic | Advanced) and a `Preferences` button.
3. **Body** — CSS grid `minmax(340px, 404px) minmax(0, 1fr)`, 2 px vertical divider. In QML: a
   `RowLayout` with the rail at `Layout.preferredWidth: 404; Layout.minimumWidth: 340` and a 2 px
   `Rectangle` separator.
4. **Prototype-states bar** — not part of the product.

---

## Screen 1 — Workbench (the main window)

### Left rail — the signal (always visible, never scrolled away by configuration)

Three numbered blocks separated by 2 px dividers. Each block header is a row: a mono ordinal in
`--color-accent-700` (11 px), an 11 px uppercase label (letter-spacing 0.12em, weight 600), and a
2 px rule filling the remaining width.

**01 / Input.** A full-width segmented control `File | Live capture` — this replaces the two adjacent
Source and Live capture cards, answering §6 Q3 (there is one input, with a source selector).

- *File branch*: `Choose WAV…` button (36 px) + the path in mono 12 px, single-line with ellipsis.
  Below, a four-column strip on a 1 px top rule: Rate `48 000`, Channels `6`, Length `0:08`,
  Bits `24` — each a 10 px uppercase label over a 13 px mono value.
- *Live branch*: endpoint `ComboBox`, then `Refresh` and **`Monitor`** buttons and a live elapsed
  readout. **This is a behaviour change**: monitoring runs the meters with no filename and no file
  written, fixing journey B's "commit to a take before you can check the signal". The note beneath
  says so in one sentence.

**02 / Levels.** Header row: layout name in Archivo 800 / 20 px, a live dot + `live` label when a run
is in flight, and a `Coded | Rendered` segmented control pushed right (§6 Q7 — the fourteen-rows-for-
twelve-speakers question becomes a mode, not a puzzle).

Meter row grid: `56px 1fr 50px 30px`, 6 px gap, 3 px between rows.
- Channel name: mono 11 px, right-aligned.
- Track: 13 px tall, `--color-neutral-200`. RMS fill `--color-neutral-800`; fill turns
  `--color-accent` once peak exceeds −6 dBFS. Peak hold: a 2 px `--color-text` bar at the peak
  position. Scale is −60…0 dBFS mapped linearly across the track (this matches the current
  `ChannelMeter.qml` mapping — keep taking positions and printed numbers from the C++ analysis layer
  so they cannot disagree).
- dB readout: mono 11 px, right-aligned, one decimal.
- CLIP box: 30 px, 8 px label, 1 px `--color-neutral-300` border.
- A channel the routing feeds nothing renders at **45 % opacity** with `-∞`.
- In Coded mode, bed channels a dependent substream replaces (`Ls (bed)`, `Rs (bed)`) are grouped
  behind a 2 px `--color-accent-300` left rule so the duplication reads as structure. Rendered mode
  hides them.

Footer line under the meters, on a rule: `All 6 coded channels fed …` in neutral-800, or — when the
source is narrower than the layout — `2 of 14 coded channels fed…` on a 2 px `--color-accent` top
rule in `--color-accent-700`. This is half the answer to §6 Q8; the other half is in Format.

**03 / Soundfield.** Two square plan views side by side, `Ear level` and `Ceiling` (§6 Q6 — a flat
ring cannot show a ceiling layer, so there are two rings). 1 px border, `--color-neutral-100` fill,
crosshair in neutral-300, a 74 % dashed circle on the ceiling plan. Speaker dots are 10 px squares:
`--color-text` for front, neutral-600 for sides, neutral-500 for rears and ceiling; the centre
speaker takes `--color-accent`. The energy vector is a 2 px accent line from the listener. Captions
are mono 10 px: `8 speakers · vector 12° front`, `4 height · silent`.

`SoundfieldView.qml` needs the ceiling plan added; the existing ring becomes the ear-level plan.

### Right panel — the stream

**Plan strip** (2 px bottom divider). Left: 10 px uppercase `The stream`, then the plan headline in
Archivo 800 / 26 px, then a mono 12 px sub-line. Right: `Tools` over the CLI tools token in mono
13 px on a `--color-neutral-200` chip.

The headline is derived, never typed: `<codec> · <shape> · <rate> kbps · <suffix>`. The sub-line is
`N speakers from M coded channels · K dependent substreams` when they differ, else `N channels, one
substream` — same strings `EncoderController::layoutDetail()` already produces.

**Tab bar** (2 px bottom divider). Tabs are text buttons, 13 px uppercase, weight 600, 28 px apart,
3 px bottom border in `--color-accent` when active and transparent otherwise; inactive tabs sit at
0.55 opacity. Tabs: **Format**, *Coding tools*, *Metadata*, **Objects**, *Live session*.
Coding tools and Metadata appear only in Advanced; Live session only when the source is live. Badge
counts (mono 10 px, accent fill, `--color-bg` text) show how many non-default settings a hidden panel
holds, so a collapsed panel still declares itself.

#### Format tab

1. **Preset + codec + bit rate + container** — a grid of `repeat(auto-fit, minmax(180px, 1fr))`,
   20 px gap, 2 px bottom rule. Presets are buttons (5.1, 7.1, 5.1.4, 7.1.4, 5.2), not a dropdown:
   they are starting points, not the model.
2. **Channels — the two-tier picker.** This replaces `layoutNames()` as a UI concept entirely; see
   *The channel model* below.
3. **Routing** — a Source → Coded strip (two cells either side of an arrow on a
   `--color-neutral-100` field, each a 10 px uppercase label over Archivo 800 / 19 px), a generated
   sentence, and a **channel map**: one tag per coded channel, `tag-neutral` (filled) when the source
   feeds it and `tag-outline` when it is carried silent, with the legend spelled out. This is the
   before-the-fact half of §6 Q8. Note there is exactly **one** generated sentence here, not two —
   the current build's two lines restate each other (brief §2).
4. **Loudness** (Basic only) — DRC profile, dialnorm (disabled while `measure` is ticked), and the
   measure checkbox. In Advanced this lives on the Metadata tab instead, so it appears exactly once.
   A ghost link `Coding tools and broadcast metadata →` switches to Advanced.
5. **Passthrough to a receiver** — endpoint combo annotated with capability, `Refresh` and `Play`
   (disabled when the endpoint cannot bitstream), and the explanatory paragraph (Advanced only).

#### Coding tools tab (Advanced only)

Four rows on 1 px rules, grid `28px 1fr 150px 200px`. Each row: a 18 px accent checkbox, the tool
name (15 px, weight 600) over a 12 px neutral-700 description, the band-edge/GAQ spin box (mono),
and the range hint in mono 11 px (`range 0–15 · auto at 0`). The spectral-extension seam checkbox is
an indented sub-row on `--color-neutral-100`. At the foot, a `Command line` strip on a 2 px accent
left rule holding `--tools cpl+spx+aht` and a Copy button — **§5's CLI parity is preserved here and
in the command bar; do not drop it.**

#### Metadata tab (Advanced only)

Two columns, 40 px gap. Left: Loudness (DRC profile, dialnorm, measure checkbox, explanation) then
Downmix (centre, surround). Right: Heavy compression (checkbox, then ceiling and dialogue target
indented behind a 2 px `--color-accent-200` left rule) then Mixing metadata (checkbox, preferred
downmix, LFE mix level, same indent). The indent rule replaces the current build's habit of injecting
controls into the middle of a column.

#### Objects tab

Header: a 52 × 28 switch, the title, the derived summary, and — when the bit rate is under 384 kbps —
the warning *with a `Set it` button that changes the bit rate from here*, since the control it refers
to is on another tab (brief §3 journey C).

Body, `340px 1fr`:
- **Room plan**, 340 × 300, 1 px border, crosshair, `front`/`rear` labels. Object markers are 14 px
  ink squares; the selected one is 18 px `--color-accent` with a 2 px `--color-text` outline at 2 px
  offset and a mono label chip. Below, x / y / z readouts to two decimals.
- **Object list** — a table: Object, Source, x, y, z, **Path**, LFE, Keys. There is **no Spread
  column and no Spread slider**: spread was standing in for per-object placement and is retired now
  that placement is per object (§6 Q5). Selected row is `--color-accent-100`.
- A `Author a path | Drive it live` segmented control (§6 Q4). Live driving requires a monitored
  capture and says so, pointing at the Live session tab, rather than offering a dead control.
- Two sliders only: **Height** (−1.00…+1.00) and **LFE send** (0.00…1.00), each a 6 px track with a
  4 × 14 px accent knob, the value in mono above and the range in mono 9 px below. Ranges and units
  are now visible, which they are not today.
- **Motion timeline** — a ruler 0…8 s, one lane per object with 8 px rotated-square keyframes joined
  by a 1 px neutral-400 line, the selected lane on `--color-accent-100`, and a 2 px accent playhead.
  `Add key` and `Preview` sit in the header with the transport readout.

#### Live session tab (live source only) — the capstone

This is prompt 4's scenario, and the surface the other pieces compose into.

- **Reconnection banner** (conditional): accent-100 field, 2 px accent left rule, stating that the
  receiver is re-locking and about a second of audio will be lost. A layout change is a deliberate,
  visible act — do not try to hide the dropout.
- **Transport**: `Stop session` primary, then Running / Frames / Dropped readouts in mono 15 px, and
  an `Also write the take to disk` checkbox.
- **Chain**: three cells inside one 1 px border, separated by arrow cells on `--color-neutral-100`.
  Capture → Live encode → Receiver leg. **The encode and the receiver leg are separate plans**: the
  encode follows the picker (e.g. `Dolby Digital Plus · 7.2.4 · 768 kbps`), the receiver leg is
  capped at what can be bitstreamed today (`Dolby Digital · 5.1 · Denon AVR-X3800H`).
- **Gap banner** (when they differ): states in words that everything past 5.1, and every object move,
  is visible on the meters and soundfield but not audible on the amplifier until DD+/Atmos
  passthrough exists. The screen has to be worth watching before that plumbing lands.
- **Live room**: 320 px tall plan, drag to move, with a `latency 42 ms` readout beside x / y / z.
- **Layout switcher**: presets; the ones above 5.1 carry a 6 px accent dot and a legend saying they
  encode and meter but leave the receiver leg at Dolby Digital 5.1.
- **Receiver reports**: Format / Input / Lock rows, where Input shows the *capped* shape and Lock
  reads `re-locking` during renegotiation.

### Run strip and command bar (bottom of the right panel, 2 px top divider)

- **Runs** — a horizontally scrolling strip of run chips, each a status square + a mono line
  (`13 · orbit51.ec3 · 768 kbps · 0:08 · 754 KB`) + an action. Encoding shows a 90 × 5 px progress
  bar and `Cancel`; failed runs show `Details`. This is §6 Q9's answer: encoding is a job with a
  history, not a modal moment.
- **Command bar** — the full `ac3cli` line in mono 12 px on a `--color-neutral-100` field with a 2 px
  `--color-text` left rule and a Copy button, then the primary **Encode** button (44 px, min 190 px,
  15 px label, play glyph). The label follows the plan: `Encode to .ac3` / `.ec3`.

Feedback (§6 Q10) now has three homes instead of one status line: field-level messages next to the
control, a banner at the top of the panel that caused the problem, and the run strip for anything
about a run.

---

## The channel model (prompts 1 and 5) — the important part

`layoutNames()` currently returns seven fixed presets. The UI concept becomes:

**Tier 1 — the bed. Exactly one, always.** There is no "no bed" state; the format cannot carry any
channel, ceiling ones included, without one.

| id | channels |
| --- | --- |
| `1/0` | C |
| `2/0` | L R |
| `3/0` | L C R |
| `2/1` | L R Cs |
| `3/1` | L C R Cs |
| `2/2` | L R Lss Rss |
| `3/2` | L C R Lss Rss |

Plus an independent **LFE** toggle that applies to any of them.

**Tier 2 — extras, added to the bed.** Each pair is a **single** toggle; half a pair does not exist.

| id | channels | label |
| --- | --- | --- |
| `wide` | Lw Rw | Front wide |
| `rear` | Lrs Rrs | Rear surround |
| `topf` | Ltf Rtf | Ceiling front |
| `topm` | Ltm Rtm | Ceiling middle |
| `topr` | Ltr Rtr | Ceiling rear |
| `lfe2` | LFE2 | Second LFE — independent, not a second sub |

**Budget: 16 positions across bed + extras combined.** An extra that would not fit greys to 0.3
opacity, refuses the tick, and prints `no budget left` on its row.

**Two separate constraints, kept apart** — conflating them was a real bug during design:
- *Dolby Digital* leaves all seven beds and the LFE toggle live and disables only the extras
  (`Dolby Digital Plus only`). The 5.1 cap then falls out naturally, because 3/2 + LFE is the widest
  bed. Do not lock the bed picker under AC-3 — `plan::carries()` already offers AC-3 mono and stereo
  and the redesign must not remove them.
- *Object mode* freezes bed, LFE and extras together at a 5.1 bed (`fixed by object mode`) and
  disables the Codec field, relabelled `Codec — fixed by object mode`.

**Derived name.** `<ear-level count>.<LFE count>[.<ceiling count>]` — so an unnamed set still reads
honestly: 3/2 + LFE + LFE2 prints `5.2`, and 3/2 + LFE + rear + ceiling front/rear prints `7.1.4`.
Coded-channel count adds 2 dependent substreams when there is a ceiling layer or more than five
ear-level channels.

**Substreams are not a UI concept.** The picker expresses a set of positions; which substream carries
what is entirely the encoder's business.

**CLI parity.** The command line follows the same two tiers:
`--bed 3/2 --lfe --extras rear+topf+topr --bitrate 768 …`. Object mode emits `--bed 5.1 --objects
--paths <file>.oamd`. Keep `ac3cli` and the GUI generated from the same tables, as §5 requires.

---

## Basic / Advanced (prompt 3)

Defaults to **Basic**.

- **Basic**: source, codec, channel picker, bit rate, output path and container, plus Loudness (DRC
  profile and dialnorm — judged basic because both have sane defaults and dialogue level is a real
  creative choice) and the Objects tab.
- **Advanced** adds the Coding tools and Metadata tabs: Annex E toggles and their band-edge/GAQ
  sub-parameters, mixing metadata, heavy compression ceiling and dialogue target, coupling
  parameters.
- Switching to Basic while on a hidden tab falls back to Format rather than showing an empty panel.
- Atmos object controls stay visible in both, on the grounds that when Atmos is on it is the point of
  the session.
- The CLI line stays visible in **both** modes; a codec developer must always be able to get back to
  a command line from what the UI shows.

---

## Interactions and behaviour

| Trigger | Result |
| --- | --- |
| Source segmented control | Switches the whole input block; meters and plan follow |
| `Monitor` (live) | Meters run, nothing written, no filename asked |
| Bed button | Sets the bed; refused when object mode is on |
| LFE toggle | Adds/removes LFE; refused when object mode is on |
| Extra row | Toggles the whole pair; refused when locked or over budget |
| Preset button | Sets bed + LFE + extras together; in a live session also triggers renegotiation |
| Preset button during a live session | Reconnection banner for ~2.2 s, Lock reads `re-locking`, then settles |
| Coded / Rendered | Shows or hides bed-replacement meter rows |
| Basic / Advanced | Shows or hides the Coding tools and Metadata tabs |
| Object switch | Fixes codec and bed, rewrites plan / suffix / CLI / encode label everywhere |
| `Set it` on the bit-rate warning | Raises the bit rate from the Objects tab |
| Encode | Adds a run to the strip with a progress bar and Cancel |

Animation: meter interpolation stays as it is today — the controller publishes ~30 snapshots/sec and
QML animates between them over 40–90 ms. Nothing in this design puts shadows, blurs or large
repaints behind the meters; keep it that way, the scene has to hold 60 fps with fourteen meters and
two soundfield plans animating.

## State

Everything the prototype holds, and roughly what it maps to:

| State | Values | Maps to |
| --- | --- | --- |
| `screen` | `work` \| `firstrun` | whether a source has ever been chosen |
| `src` | `file` \| `live` | the unified input selector |
| `tab` | `format` \| `coding` \| `meta` \| `objects` \| `session` | right-panel tab |
| `expertise` | `basic` \| `advanced` | persisted preference |
| `meterMode` | `coded` \| `rendered` | persisted preference |
| `bed` | one of the seven ids | `EncoderController` bed |
| `lfe` | bool | LFE present |
| `extras` | array of extra ids | extras present |
| `objectsOn` | bool | `atmos_enabled_` |
| `objMode` | `author` \| `live` | object surface mode |
| `reconnecting` | bool | receiver renegotiation in flight |
| `encoding` / runs | — | the run list |

Derived, never stored: the shape name, coded/rendered counts, the plan headline and sub-line, the
encode label, the CLI line, the routing sentence, the passthrough (capped) shape, and every
"is this control available" flag.

---

## Screens 2–4 — first run, preferences, error

**First run.** Two equal columns. Left: kicker `First run`, an Archivo 800 / 52 px headline "Bring in
some audio.", a 15 px paragraph, then three full-width 52 px flush-left buttons (Choose a WAV file… /
Capture from a device / Open the bundled 5.1 test signal). Right, on `--color-neutral-100`: three
numbered rows on 2 px rules explaining the window in one sentence each, and a closing note that
advanced controls start hidden.

**Preferences** — a modal on the standard backdrop, 760 px wide, 2 px `--color-text` border, no
radius. Two columns: *Appearance* (theme Light/Dark/System, a note that meter colours invert but
thresholds do not move, controls-shown-on-open, meters-follow) and *Defaults for a new run* (codec,
container) + *Capture* (start monitoring on selection, ask for a filename before recording) +
*Command line* (keep the `ac3cli` line visible). Cancel / Save in a footer on a 2 px rule.

**Error** — three parts, none of them a shared status line: a banner at the top of the right panel
(accent-100 field, 2 px accent bottom rule, warning glyph, a bold sentence naming the run and what
stopped it, a plain-language explanation, and two actions — `Choose another device`, `Retry as file`);
a failed chip in the run strip; and, where relevant, the field itself. Partial output is named and
kept, not silently discarded.

---

## Design tokens

From the Modernist system. Zero radius everywhere — this is deliberate, do not round anything.

**Colour**

| Token | Value |
| --- | --- |
| bg | `#f3f2f2` |
| surface | `#eae9e9` |
| text | `#201e1d` |
| accent | `#ec3013` |
| divider | `color-mix(in srgb, #201e1d 40%, transparent)` |
| neutral 100…900 | `#f8f4f4` `#eae7e7` `#d7d3d3` `#bab6b6` `#9b9797` `#7d7979` `#605d5d` `#444141` `#2d2b2b` |
| accent 100…900 | `#fff2ef` `#ffe0d9` `#ffc4b8` `#ff9783` `#ff563c` `#dd2b0f` `#ae1800` `#7c1405` `#4d170e` |

Accent-on-ground is tuned to ≥3:1 — fine for icons, large text and chrome, **not** for body copy. Use
`--color-accent-700` for paragraph-size text in the accent.

**Type** — Archivo throughout; headings weight 800, body 400/500/600. Sizes in use: 52 (first-run
headline), 26 (plan headline), 22 (wordmark), 20 (layout name, dialog title), 19, 17, 15, 14 (body),
13, 12, 11 (uppercase labels, mono readouts), 10 (kickers), 9 (scale ticks). Numeric readouts are
monospace — `ui-monospace, "Cascadia Mono", "SF Mono", monospace` in the prototype; use whatever
fixed-width face the app already ships so digits do not shift.

**Spacing** — 4 / 8 / 12 / 16 / 24 / 32. **Radius** — 0. **Elevation** — `--shadow-sm/md/lg`; the
only thing that uses one is the Preferences dialog.

**Rules** — 2 px `--color-divider` between major regions, 1 px inside a region. Do not soften these
into hairlines or replace them with whitespace; the structure is doing the organising.

**Theming.** Both themes must come from the same tokens, and the Fusion palette must be set from
whichever theme is active — the current build styles only the custom-drawn parts, leaving Fusion's
own pale pink on every switch, checkbox, slider and progress bar. That is the single most visible
inconsistency today (brief §2) and this redesign is not implemented until it is gone. Meter
thresholds (−6, −1, full scale) do not move between themes; only their colours invert.

## Assets

Icons are Lucide (https://lucide.dev), drawn inline at 15–20 px, 2 px stroke, `currentColor`: folder,
mic, disc, settings, arrow-right, play, pause, check, alert-triangle, info. No raster assets, no
photography, no emoji.

## Files

- `ac3forge Workbench.dc.html` — the full prototype, all six states.
- `docs/DESIGN-BRIEF.md` (in the repo) — the current-state inventory this redesign answers.

## Suggested order of work

1. Theme first: tokens + light/dark + the Fusion palette. Nothing else looks right until this lands,
   and it is independent of every other change.
2. The shell: window, header, two panes, tab bar, plan strip, command bar. Move the existing cards in
   as-is, unchanged, so the app is never broken mid-refactor.
3. The channel model — controller and picker together, since it changes `layoutNames()`'s role.
4. Meters (coded/rendered, the fed/unfed footer) and the ceiling soundfield plan.
5. Runs and the three feedback homes.
6. Basic/Advanced gating.
7. Objects: per-object placement and paths, spread removed.
8. Live session, last — it composes everything above and depends on live-monitor plumbing.

## Open questions for you

- The picker enforces bed + extras + a 16-position budget. If there are further format rules
  (extras that require a particular bed, ceiling combinations that cannot coexist), say so and they
  should be made unreachable rather than merely discouraged.
- The 720 × 560 minimum window is not compatible with this content; 1280 × 900 is the honest floor.
- Object paths need a storage format. The CLI line assumes a sidecar (`--paths <file>.oamd`); if
  paths should live in the project state instead, the command bar's line changes with it.
