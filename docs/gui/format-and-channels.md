# Format & channels

The Format tab is the one tab always present in Advanced and Expert — Guided covers the same
state through its Speakers and Quality steps instead (see
[Guided, Advanced, Expert](index.md#guided-advanced-expert)). This page describes the tab, top to
bottom: presets and the derived codec, the two-tier channel picker, the routing strip, and the
assignment table.

## Presets, codec, bit rate, container

A row of layout **presets** (5.1, 7.1, 5.1.4, 7.1.4, 5.2, 7.2.4 — starting points, not the
model: they set the bed, LFE count and extras together, but the channel picker below is what the
encode plan actually reads), then **Codec**, **Bit rate**, and **Container** (elementary stream
or Matroska):

![E-AC-3, 7.1.4 preset, rear + ceiling extras on](screenshots/format-eac3-714.png)

**The codec is derived, never chosen.** A bed with or without an LFE is Dolby Digital; *any*
extra — rear, ceiling, a second LFE — promotes the stream to Dolby Digital Plus. The Codec field
is therefore labelled *Codec — follows the channels*, shows the derived value, and is disabled
(in object mode it reads *Codec — fixed by object mode*). This replaces the old
choose-a-codec-first flow, which gated the extras on a codec the extras themselves change.

The plan strip above the tabs updates live: `E-AC-3 · 7.1.4 · 192 kbps · .ec3` (or `quality <n>`
in VBR mode), with a sub-line counting what differs when it does (`12 speakers from 12 coded
channels · 2 dependent substreams` — see
[Metadata options](../cli/metadata-options.md#the-layout-grammar)).

## Rate mode: CBR or VBR

E-AC-3 only, and file output only — the control disappears entirely for AC-3 (no free word count
to vary; `frmsizecod` indexes a fixed table) and during a live session (IEC 61937 passthrough
bursts are fixed-size per access unit — see
[Live capture & session](live-session.md#the-vbr-warning)):

![Rate mode set to VBR: quality 75, no floor, an upper bound at 640 kbps](screenshots/format-vbr.png)

**CBR** (the default) is the plain **Bit rate** dropdown above. **VBR** replaces it with a
**Quality** slider, 0 (smallest) to 100 (best) — encoder-relative, not a fixed target, and *not*
linear in bit cost: cost rises steeply above roughly half the range, so a high quality with no
upper bound will often refuse real programme material outright (`FrameError::kInvalidBitrate`)
rather than silently producing an oversized frame. Two checkboxes, **Minimum** and **Maximum**,
each reveal a kbps field when ticked — presence lives on the checkbox, never a sentinel value:
unticked means no bound at all in that direction, not a default one. The line beneath states the
current bounds in words (`no floor · ≤ 640 kbps`). **Bit rate** above still matters in VBR mode —
it keeps feeding the same coupling/spectral-extension band-edge defaults it always has, just not
as a target rate.

A finished VBR run reports what it actually spent, since it has no target: the run strip reads
`VBR q75 · avg 512 kbps (384–704)` instead of a plain `NNN kbps` figure. At the foot of the
panel, a monospace `ac3cli vbr token` readout shows the exact
`q:<quality>[,min:<kbps>][,max:<kbps>]` string that reproduces the current setting — see
[CLI → Metadata options](../cli/metadata-options.md#the-vbr-token-eac3-encode-only).

## Channels — the two-tier picker

A budget counter in the section header (`12 of 16 positions used · 7.1.4` — Table E2.5's channel
space) tracks the whole selection. Beneath it, the two tiers:

1. **Bed — pick one.** Eight buttons: `1+1` (dual mono, drawn apart from the other seven), then
   `1/0` through `3/2`, each showing its channel names. There is no "no bed" state — the format
   cannot carry any channel, ceiling ones included, without one.
2. **Low frequency — a count, not a flag.** Three buttons: **None**, **One · LFE**, **Two ·
   LFE + LFE2**. Two means two *independent* low-frequency channels carrying different signal —
   not one signal sent to two subwoofers — which is what makes a 7.2.4 rather than a 7.1.4 (and,
   like everything past a bed and its LFE, needs Dolby Digital Plus).
3. **Extras — added to the bed.** Four checkbox rows — front wide, rear surround, ceiling front,
   ceiling rear — each a *pair* that toggles together (you can't add a left ceiling channel
   without its right pair). A row that can't currently be ticked says why in its own right-hand
   column: `Dolby Digital Plus only` under a plain bed, `fixed by object mode`, `no budget left`
   at the 16-position cap, or (when unticked under AC-3) `moves to Dolby Digital Plus` — the cost
   stated only while it is actually true.

    !!! note "No ceiling middle"
        The design handoff sketched a third ceiling pair ("ceiling middle"). A/52 Table E2.5 has
        no such location — only Vhl/Vhr and Lts/Rts pairs exist — so it is not offered rather
        than invented.

The derived shape name (`5.1`, `7.1.4`, `5.2`, …) follows the selection —
`<ear-level count>.<LFE count>[.<ceiling count>]` — so an unnamed combination still reads
honestly. Substreams are not a UI concept: the picker expresses a set of positions, and which
substream carries what is the encoder's business.

### Dual mono

Not a speaker layout at all — two independent, single-channel programmes sharing one syncframe
(§5.4.2's "1+1 dual mono"). Selecting it clears the LFE and extras and greys those controls with
the reason (`not part of dual mono`); the soundfield plans are replaced with two named programme
cards, and the meters read **Program 1 / Program 2** — never a correlated pair:

![1+1 selected: LFE count and extras locked, programme cards in the rail](screenshots/format-dual-mono.png)

The two programmes' channels come from either one two-channel WAV (automatic: ch 1 → programme 1,
ch 2 → programme 2) or any loaded channels assigned `Programme 1` / `Programme 2` in the
[assignment table](source-assignment.md). Each programme gets its own **dialnorm** on the
[Metadata tab](metadata.md#loudness) (`dialnorm2` for programme 2); automatic `dialnorm=auto`
measurement is not yet supported for dual mono, so both have to be set by hand.

## Routing

A **Source → Coded** strip (`2 sources · 8 ch → 5.1`), a generated sentence describing what the
routing actually does (naming any coded position carried silent), and a **channel map**: one tag
per coded position, filled when a source feeds it and outlined when it is carried silent — the
before-the-fact half of the same answer the meters' fed footer gives during a run.

## Assignments

The full per-channel assignment table lives here, for any number of sources — one row per loaded
channel with a destination dropdown. With one source and nothing set, rows read **Automatic**
(the routing panned them for you); everything else is explicit. This is the model the meters, the
soundfield, the routing sentence and the CLI line all derive from — it has its own page:
[Multi-source & assignment](source-assignment.md).

## Loudness and passthrough

**Advanced** adds a **Loudness** section here (DRC profile and dialnorm only — the rest of
[Metadata](metadata.md) lives on its own tab in Expert, which absorbs Loudness so it appears
exactly once), with a ghost link jumping to Expert. **Passthrough to a receiver** appears in both
Advanced and Expert — a device dropdown annotated with what each endpoint can bitstream,
**Refresh**, and **Play** for sending the last encode straight out as IEC 61937 bursts:

![Advanced tier, post-run: fed meters, Loudness section, completed run in the strip](screenshots/format-advanced-postrun.png)

![Expert tier: the full Format tab down to Passthrough](screenshots/format-expert-passthrough.png)

An E-AC-3 stream is refused at the Play button outright, since the packer only emits AC-3 bursts
(data type 1). See [Live capture & session](live-session.md) for the live equivalent, and
[Platform notes](../platforms/windows.md) for which platforms have this hardware-confirmed.

## Next

- [Multi-source & assignment](source-assignment.md) — the table everything above derives from.
- [Coding tools](coding-tools.md) — Annex E tools, Expert + E-AC-3 only.
- [Metadata](metadata.md) — the rest of the loudness/downmix picture, Expert only.
- [Objects & motion](objects-and-motion.md) — turning this same bed into an Atmos carrier.
