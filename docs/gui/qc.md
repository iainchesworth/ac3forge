# QC a stream

Roadmap C3 — the same verification `ac3cli qc` runs on the command line, reachable from the
window itself. Everything else in this guide configures and runs an **encode**: a source is
loaded, a plan is built on it, Encode writes a new file. QC is the opposite shape — an
**already-encoded** `.ac3`/`.ec3` file is opened, decoded and measured against its own embedded
`dialnorm`/`compr`, with no source, no plan and no encoder involved anywhere in the path. Folding
that into the encode workflow's tab bar (Format, Coding tools, Metadata, Objects, Live session —
every one of them a page of controls for a plan still to come) would put a page with nothing to
configure next to five pages that are nothing else, so it lives instead as its own dialog, opened
from a **QC a stream…** button in the header beside **Preferences** — the same "distinct surface,
reachable from the header, not living in the tab structure" shape [Preferences](index.md#preferences)
and a run chip's own details popover already use in this window.

## Opening it

**QC a stream…**, top right, next to Preferences and the Guided/Advanced/Expert control. It opens
regardless of what (if anything) is loaded in the main workbench — QC-ing a finished file has
nothing to do with whatever source is currently being configured for a new encode, and the two
never interact.

**Choose file…** opens a standard file picker filtered to `*.ac3`/`*.ec3` (plus **All files**).
Picking one starts the measurement immediately, the same way `ac3cli qc <file>` runs the moment
it is invoked — there is no separate "Run" step. A long file measures off the window's own event
loop (the same background-worker pattern every encode in this app already uses), so the dialog
stays responsive and shows a spinner beside the path while it works.

## The report

Once a file has been measured, the dialog fills with:

- **A summary line** — codec, layout, sample rate, unit count and duration, `ac3cli qc`'s own
  opening line restated (`AC-3 · 5.1 · 48000 Hz · 500 frame(s) · 16.00 s`).
- **One card per programme** — one, except for a `1+1` dual-mono stream, which reports Ch1 and
  Ch2 independently (§E1.3: two unrelated programmes sharing a syncframe, never averaged
  together — the same split the dual-mono rail row and Metadata tab already draw). Each card
  carries:
    - **Three meters** — integrated loudness, loudness range and true peak, drawn as a filled bar
      against a fixed scale rather than a bare number, the same "value on a track" shape
      [`ChannelMeter`](loading-a-source.md#02-levels) uses for the workbench's own channel levels.
      With a single delivery preset selected (see below), the loudness meter also draws a shaded
      **tolerance band** around that preset's target and the true-peak meter a **ceiling line** at
      its limit — the value's own fill turns from neutral to the app's accent colour exactly when
      it falls outside the band or crosses the ceiling, the same two-state colouring the channel
      meters' own CLIP indicator uses. Loudness range has no preset gate (none of the three named
      presets define one), so its meter always reads as a plain measurement.
    - **A dialnorm check** — the stream's own `dialnorm` restated as the LKFS it claims
      (`§5.4.2.8`: dialnorm is how far dialogue sits below digital 100%), the measured-minus-claimed
      delta, and what dialnorm the measurement itself would imply — exactly the three lines
      `ac3cli qc` prints under "dialnorm check", plus `compr` (§5.4.2.9/§7.7.2) when the stream
      carries one.
    - **A verdict row per delivery preset** — EBU R 128 s2, ATSC A/85 and Netflix (the same three
      `ac3::meta::qc.hpp` names, each preset's target/tolerance/ceiling cited from its own primary
      source — see that header's own comment for the exact clauses), each showing its own
      loudness PASS/FAIL, true-peak PASS/FAIL and an overall chip.

## Delivery preset

A segmented control — **All**, **EBU R 128 s2**, **ATSC A/85**, **Netflix** — mirrors `ac3cli
qc`'s own `preset=<name>|all` argument:

- **All** (the default) lists every preset's own verdict, with no single target/ceiling to draw
  as a line on the meters above (three different presets would mean three different bands on the
  same bar) — the meters show plain measured values, and the report becomes a compact overview of
  where the stream sits against all three deliveries at once.
- Choosing **one** preset narrows the verdict list to it and feeds that preset's own numbers into
  the meters as the tolerance band / ceiling line, so the report can show exactly *why* a gate
  passed or failed rather than only *that* it did.

## What it does not do

This reads a stream that already exists; it has no connection to the workbench's own encode
plan, the loaded source, or the run strip. QC-ing the file a run just produced is a matter of
choosing it from the file picker like any other — there is no shortcut yet from a finished run
chip straight into this dialog.
