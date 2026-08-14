# ac3gui — window layout

`ac3gui` (window title `ac3forge`, QML module `Ac3Forge`) is a Qt Quick front end over the same
`ac3::forge` library documented under [Library](../library/index.md) — nothing in the GUI has
logic the library doesn't also expose, and every setting it makes maps onto an equivalent
[`ac3cli`](../cli/index.md) invocation shown live at the bottom of the window.

The screenshots in this guide are of the current two-pane "workbench" layout, drawn in the
Modernist design system's light palette (a dark theme is derived from the same tokens and follows
the OS, or the Theme preference — see [Preferences](#preferences)). Earlier builds — a nine-card
single-column design, and a first cut of the workbench before the design handoff was fully
implemented — are superseded; if you find references to either elsewhere in the repo's history,
they predate this guide.

## First run

Until a source has ever been chosen, the window shows a first-run screen instead of the workbench
— three ways in (a WAV file, a capture device, or a bundled 5.1 test signal the app synthesises
on the spot) and a one-sentence tour of the window:

![The first-run screen](screenshots/firstrun.png)

## The window

Minimum size 1280×900. Two panes, divided by a vertical rule:

![The workbench: a 5.1 source loaded, Advanced tier](screenshots/overview-default.png)

- **Header** (top): the `ac3forge` wordmark and subtitle, a **Guided / Advanced / Expert**
  segmented control, and a **Preferences** button.
- **Left rail — "the signal"** (always visible, never scrolled away, and never affected by which
  tier is selected): three numbered blocks — **01 Input** (one input, with a **File / Live
  capture** selector, the loaded source list and its totals), **02 Levels** (the channel meters),
  and **03 Soundfield** (the plan views). This is what's coming *in* — see
  [Loading a source](loading-a-source.md).
- **Right panel — "the stream"**: a plan strip showing the derived output headline
  (`<codec> · <shape> · <bitrate> kbps · .<suffix>`, or `quality <n>` in VBR mode, or
  `5.1 bed + <n> objects` in object mode), a sub-line counting speakers, coded channels and
  dependent substreams, and the Annex E tools token on a chip. Beneath it, a tab bar (hidden in
  Guided, which fills the panel with its own steps) — tabs carry a badge counting their
  non-default settings, so a collapsed panel still declares itself.
- **Run strip** (bottom): past and in-flight runs — file encodes, recordings, and real live
  sessions alike — a live-generated `ac3cli` command line with a Copy button, and the primary
  Encode button. Present in every tier, including Guided — a codec developer must always be able
  to get back to a command line from what the UI shows. The line is genuinely complete: extra
  sources ride as `src=`, the assignment as `map=`, non-default metadata in `print_meta_usage`'s
  own grammar, AC-3's bare `couple`, quoting where names carry spaces; a live source renders the
  `live` subcommand, and a Matroska container is honestly *two* commands (`… && ac3cli mkv …`),
  because pasting one would write a raw elementary stream into a file named `.mkv`. Finished
  chips carry **Show in folder** and **Play** — sends that run's own output to a receiver over the
  same IEC 61937 passthrough path the Format tab's own Passthrough section uses (see [Format &
  channels](format-and-channels.md#loudness-and-passthrough)), greyed out when no device here can
  bitstream what that particular run actually produced. A run encoded through Guided's **Play it on
  my receiver** destination (step 5) carries the device Guided already auto-picked along with it, so
  its Play needs no fresh device pick. **Clicking a chip** (anywhere but its buttons) opens that
  run's own details popover — status, rate, duration, size, frame count, the failure text if it
  failed, and the exact `ac3cli` command line as it stood *when that run started*, snapshotted
  rather than read live, so an old chip's popover still shows what actually ran even after the
  command bar above has since moved on. Failed and cancelled chips also say which they are, in the
  chip text itself, with a frame count and a distinct square; a failure's banner names the cause
  first and offers **Choose another device** / **Retry as file**; and a refusal that never opened a
  run (an incomplete assignment, the sixteen-object cap) lands in the same banner instead of only a
  status line. The strip's last thirty finished runs persist across a restart, restored alongside
  the "reopen the last session's sources" preference below.

## Guided, Advanced, Expert

- **Guided** (the default for a new session) replaces the tabbed right panel with a five-step
  sequence — **Audio**, **Speakers**, **Quality**, **Movement**, **Where it goes** — that reads
  and writes the exact same state Advanced and Expert do. There is no separate "wizard draft":
  switch tiers mid-session and whatever guided set is exactly what Advanced or Expert already
  show for the same field, and vice versa.

  ![Guided step 1 — Audio, with "What each sound does"](screenshots/guided-wizard-source.png)

  Guided is not a dead end and not a reduced feature set: step 1 carries its own **What each
  sound does** list — the same per-channel destination dropdowns as the
  [full assignment table](source-assignment.md), in plain language — and a jump to that table
  with a lossless **Back to guided** return. Step 2's speaker cards can open a **room picker**
  sub-screen (say what's *in the room*; the channel layout falls out of the parts). Step 3's
  **Good / Better / Best** rate cards set a fixed CBR bit rate (192 / 448 / 768 kbps) normally, or
  — when a VBR default (see Preferences below) or an already-selected Variable rate mode applies —
  a VBR quality target instead (40 / 75 / 90), since a fixed bit rate is not what either of those
  is actually asking for. Step 4's movement cards drive [object mode](objects-and-motion.md), with
  trajectory presets that author real keyframes; once objects are on, two more cards ask **what
  should move** — *Everything moves* rewrites every loaded channel's assignment to an object (no
  bed left underneath them), while *Keep the bed, add movers* leaves an existing mix exactly where
  it is and only turns still-unassigned channels (a file added since) into objects — both edit the
  same [assignment table](source-assignment.md) step 1 shows, so a later hand edit there always
  sticks. Step 5's **Play it on my receiver** destination auto-picks the first output device that
  can actually bitstream what is about to be encoded (the same "AC-3 + E-AC-3 ready" capability
  labelling the Format tab's own passthrough picker uses — see [Format &
  channels](format-and-channels.md#loudness-and-passthrough)), with a **Choose a different
  device →** link to override it and a stated reason when nothing here qualifies; encoding writes
  straight to the planned filename (no save dialog, since the real destination is the receiver) and
  the finished run's own **Play** action reuses that same device. Constraints apply the same way as
  everywhere else — they are explained rather than hidden (turning movement on says it fixed the
  bed at 5.1, rather than silently locking controls elsewhere).
- **Advanced** shows a tabbed right panel — [Format](format-and-channels.md) (presets, the
  channel picker, routing, the assignment table, a Loudness section) and
  [Objects](objects-and-motion.md).
- **Expert** adds the [Coding tools](coding-tools.md) and [Metadata](metadata.md) tabs (Metadata
  absorbs the Loudness section, so it appears exactly once). [Live session](live-session.md)
  joins the tab bar in Advanced and Expert whenever the live source is selected in the rail —
  sessions running or not — and carries a `live` badge while one runs.

Switching tiers never discards anything already set — it only changes what's visible (and, for
Guided, how it's presented: one question at a time instead of a page of controls). Leaving Expert
while a tab only it shows is current falls back to Format rather than showing an empty panel.

## The loudness contract

The app's own defaults are spec-neutral — dialnorm 31, no DRC, no measurement — the same values a
plan carries if nothing here ever touched it. Guided, while it is driving, layers a stronger
default on top: measured loudness and film-standard DRC, applied automatically once the flow
reaches its "What you are about to make" summary, so the summary already tells the truth before
Encode is ever pressed. This never overwrites an actual edit — the moment Loudness/Metadata is set
by hand (in Guided itself, or in Advanced/Expert during the same session), the contract steps aside
for good, this session, and dialnorm/DRC stay exactly what was set.

Dual mono (`1+1`) gets the honest, partial version: automatic loudness measurement is refused
outright for dual mono (see [Format & channels](format-and-channels.md#dual-mono)), so the contract
never turns it on there — it applies film-standard DRC to both programmes instead (Ch2's own
`drc2`, not inherited from Ch1's — see [Metadata](metadata.md#loudness)) and leaves dialnorm to be
set by hand, the way it always has been for `1+1`.

## Preferences

A real dialog, persisted across sessions (QSettings), three columns — every row wired to real
behaviour:

- **Appearance** — theme (Light / Dark / System); which meter rows to show by default
  ([Coded / Rendered](loading-a-source.md#channel-levels)); **Explanations** — show the
  plain-language notes beside controls, and optionally warn before a choice changes the codec
  (the codec follows the channels either way; the warning only makes the moment deliberate).
- **When ac3forge opens** — the Controls tier (including "whatever I used last"); reopen the
  last session's sources and assignments (saved as one unit on close, restored on open — a file
  gone missing fails its load with the usual message rather than aborting the rest); optionally
  start on the last screen. **Files and runs** — the output folder ("beside the first source" by
  default), the `{source}.{ext}` naming pattern every save dialog and auto-named take follows,
  and keep-partial-output: a failed or cancelled run's frames land beside the intended output as
  `<name>.partial.<ext>`, named and kept, never silently discarded.
- **Defaults for a new encode** — container, rate mode, bit rate, VBR quality, DRC profile,
  measure loudness. The codec is deliberately **not** a default — it follows the channels (see
  [Format & channels](format-and-channels.md)), and a stale default would contradict that.
  Clicking **Save** applies a changed default to whichever of these fields nothing has explicitly
  touched *this session* yet — the same contract [the loudness contract](#the-loudness-contract)
  already gives DRC profile and measure loudness, generalised to container/rate mode/bit
  rate/VBR quality too. An edit made before Save is never clobbered by it: touch a field once (in
  Guided, Advanced or Expert, any of them count) and Preferences stops overwriting it for the rest
  of the session, exactly as the loudness contract already promises for Loudness/Metadata.
  **Capture** — start monitoring as soon as a device is chosen, and whether Record asks for a
  filename or writes straight to the output folder under a timestamped take name. **Command
  line** — keep the `ac3cli` line visible.

## Next

Walk the panes in the order a first encode actually goes:

1. [Loading a source](loading-a-source.md) — pick a WAV (or several), or capture live; watch the
   channel meters
2. [Format & channels](format-and-channels.md) — layout, dual mono, VBR, bit rate, container —
   and the assignment table everything else derives from
3. [Coding tools](coding-tools.md) — Annex E tools (Expert, E-AC-3 only)
4. [Metadata](metadata.md) — loudness, downmix, heavy compression (Expert)
5. [Objects & motion](objects-and-motion.md) — Dolby Atmos objects
6. [Live capture & session](live-session.md) — capture → encode → monitor/passthrough, live

Loading more than one source at once — each channel individually assigned to a bed position, an
object, or a dual-mono programme — is its own page:
[Multi-source & assignment](source-assignment.md).

Or start with [Concepts](../concepts/index.md) if terms like "dependent substream" or "JOC" are
unfamiliar — the GUI uses the same vocabulary as the standards it implements.
