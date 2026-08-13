# Format & channels

The Format tab is the one tab always present in Advanced and Expert — Guided covers the same
state through its Speakers and Quality steps instead (see
[Guided, Advanced, Expert](index.md#guided-advanced-expert)). This page describes the tab, top to
bottom: presets and the derived codec, the two-tier channel picker, the routing strip, and the
assignment table.

## Presets, codec, bit rate, container

A row of layout **presets** (5.1, 7.1, 5.1.4, 7.1.4, 5.2, 7.2.4 — starting points, not the
model: they set the bed, LFE count and extras together, but the channel picker below is what the
encode plan actually reads), then **Codec**, **Bit rate**, and **Container** (elementary stream,
Matroska, or S/PDIF — see below):

![E-AC-3, 7.1.4 preset, rear + ceiling extras on](screenshots/format-eac3-714.png)

**The codec follows the channels.** *Any* extra — rear, ceiling, a second LFE — needs Dolby
Digital Plus, so ticking one under plain AC-3 *promotes the codec on the spot* (with a
confirmation first, if the [Explanations preference](index.md#preferences) asks for one); while
anything is forcing it, the field reads *Codec — follows the channels* (or *fixed by object
mode*) and is disabled. With nothing forcing it — a plain bed, with or without its LFE — the
choice is real (both codecs genuinely carry it, and VBR needs E-AC-3), so the field is live
there. What never happens is the old circular gate, where extras were locked behind a codec the
extras themselves change.

The plan strip above the tabs updates live: `E-AC-3 · 7.1.4 · 192 kbps · .ec3` (or
`quality 75 · ≥192 ≤640` in VBR mode, bounds included), with a sub-line counting what differs
when it does (`12 speakers from 12 coded channels · 2 dependent substreams`; in object mode it
counts the fed bed positions live — `4 of 6 bed positions fed · JOC + OAMD · objects carry the
height` — even mid-drag). See
[Metadata options](../cli/metadata-options.md#the-layout-grammar).

The **Bit rate** list carries the 19 nominal AC-3 rates plus a 768 kbps rung that exists for
E-AC-3 only — E-AC-3 signals its frame size directly rather than indexing Table 5.18, and a wide
object or 7.2.4 session genuinely wants it. Switching back to AC-3 clamps an over-table rate to
640 rather than leaving a plan `validate()` would refuse at encode time.

A muted line can appear under the field itself: *"N coded channels at M kbps will audibly
starve — encoders refuse outright below the frame minimum."* This is a field-level hint, not a
second gate — it fires when the rate works out to less than roughly 77 kbps per full-bandwidth
coded channel (LFE excluded), the same per-channel rate the object-mode 384 kbps advice already
implies for a 5.1 bed (384 / 5). Nothing here refuses an encode; a deliberate stress test at an
implausibly low rate still runs, right up to the frame's own hard minimum, which is a separate,
harder line enforced only at encode time. Object mode's own 384 kbps warning (below) takes over
instead of this one whenever objects are in play, so the two are never shown together. Guided's
Quality step shows the same idea in plain language when a wide room still has "Good" selected.

**Container**'s third option, **S/PDIF (.wav)**, wraps the encoded stream's IEC 61937 bursts —
exactly what `ac3cli spdif` produces from a finished file — as a 2-channel 16-bit PCM WAV, playable
bit-exactly (100% volume, no mixing) into an S/PDIF or HDMI output so a receiver locks onto it and
lights up its Dolby Digital indicator. Works for both codecs: an E-AC-3 stream's carrier runs at
four times the content sample rate (Dolby Digital Plus over IEC 60958/61937), which is legal and
expected, if unusual for a plain PCM16 file. Like Matroska, this container is not something the
encoder itself writes in one step — the copyable command line is honestly two commands
(`ac3cli encode … out.ac3 && ac3cli spdif out.ac3 out.wav`), because pasting one command would
otherwise write a raw elementary stream into a file the receiver expects to be a WAV.

## Rate mode: Constant or Variable

E-AC-3 only, and file output only — the control disappears entirely for AC-3 (no free word count
to vary; `frmsizecod` indexes a fixed table) and whenever the **live source is selected** (IEC
61937 passthrough bursts are fixed-size per access unit — see
[Live capture & session](live-session.md#the-vbr-warning)):

![The rate-mode panel absent while a live source is selected](screenshots/format-vbr.png)

**Constant** (the default) is the plain **Bit rate** dropdown above. **Variable** adds a
**Quality** slider, 0 (smallest) to 100 (best) — encoder-relative, not a fixed target, and *not*
linear in bit cost: cost rises steeply above roughly half the range, so a high quality with no
upper bound will often refuse real programme material outright (`FrameError::kInvalidBitrate`)
rather than silently producing an oversized frame. Two checkboxes, **Set a minimum bit rate**
and **Set a maximum bit rate**, each reveal a kbps field when ticked — presence lives on the
checkbox, never a sentinel value: *"Bounds are optional — unticked means no bound at all, not a
default one"*, as the line beneath says, before stating the current bounds in words. **Bit
rate** above still matters in VBR mode — its label relabels itself *band-edge reference, not a
target*: it keeps feeding the same coupling/spectral-extension band-edge defaults it always
has.

A finished VBR run reports what it actually spent, since it has no target: the run strip reads
`VBR q75 · avg 512 kbps (384–704)` instead of a plain `NNN kbps` figure. At the foot of the
panel, a monospace `ac3cli vbr token` readout shows the exact
`q:<quality>[,min:<kbps>][,max:<kbps>]` string that reproduces the current setting — see
[CLI → Metadata options](../cli/metadata-options.md#the-vbr-token-eac3-encode-only).

## Channels — the two-tier picker

A budget counter in the section header (`12 of 16 positions used · 14 coded channels` — Table
E2.5's channel space on one side, what the stream actually transmits on the other) tracks the
whole selection. Beneath it, the two tiers:

1. **Bed — pick one.** Eight buttons: `1+1` (dual mono, drawn with a dashed border because it is
   categorically different — two programmes, not a speaker shape), then `1/0` through `3/2`,
   each showing its channel names. There is no "no bed" state — the format cannot carry any
   channel, ceiling ones included, without one.
2. **Low frequency — a count, not a flag.** Three buttons: **None**, **One · LFE**, **Two ·
   LFE + LFE2**. Two means two *independent* low-frequency channels carrying different signal —
   not one signal sent to two subwoofers — which is what makes a 7.2.4 rather than a 7.1.4 (and,
   like everything past a bed and its LFE, needs Dolby Digital Plus).
3. **Extras — added to the bed.** Four checkbox rows — front wide, rear surround, ceiling front,
   ceiling rear — each a *pair* that toggles together (you can't add a left ceiling channel
   without its right pair), each printing the channel tokens it adds (`Lw Rw`) in the same
   Table E2.5 names the channel map uses. A row that can't currently be ticked says why in its
   own right-hand column: `fixed by object mode`, `not part of dual mono`, `no budget left` at
   the 16-position cap, or (when unticked under AC-3) `moves to Dolby Digital Plus` — the cost
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
the reason (`not part of dual mono`); an accent note under the picker says what 1+1 is *for*
(a second language, a commentary track — chosen by the listener, never mixed); the routing
sentence states the multiplex plainly; the channel map shows the two `p1`/`p2` tags; the
soundfield plans are replaced with two named programme cards; and the meters read
**Program 1 / Program 2** — never a correlated pair:

![1+1 selected: LFE count and extras locked, programme cards in the rail](screenshots/format-dual-mono.png)

The two programmes' channels come from either one two-channel WAV (automatic: ch 1 → programme 1,
ch 2 → programme 2) or any loaded channels assigned `Programme 1` / `Programme 2` in the
[assignment table](source-assignment.md). Each programme gets its own **dialnorm**, **DRC
profile**, and (Expert) **heavy compression** on the [Metadata tab](metadata.md#loudness)
(`dialnorm2`/`drc2`/`heavy2` for programme 2) — the two programmes are unrelated, so programme 2's
curve is never inherited from programme 1's; a plan that wants both compressed alike sets both
explicitly. Automatic `dialnorm=auto` measurement is not yet supported for dual mono, so dialnorm
has to be set by hand on both — DRC and heavy compression have no such restriction.

## Routing — what happens to this source

A **Source → Coded** strip (`2 sources · 8 ch → 8 coded · 6 spk` — the coded/speaker split, so a
dependent substream's replaced channels stop being invisible bookkeeping), a generated sentence
describing what the routing actually does (naming any coded position carried silent), and a
**channel map**: one tag per coded position, filled when a source feeds it and outlined when it
is carried silent — the before-the-fact half of the same answer the meters' fed footer gives
during a run.

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

**Play greys out for an endpoint that cannot bitstream the encoded stream** — the device labels
already say what each accepts (`AC-3 + E-AC-3 ready`, `cannot bitstream`, …), and the button
reads them rather than failing after the click. AC-3 rides data-type-1 bursts and E-AC-3
data-type-21 bursts at four-times rate. See [Live capture & session](live-session.md) for the
live equivalent, and [Platform notes](../platforms/windows.md) for which platforms have this
hardware-confirmed.

## Next

- [Multi-source & assignment](source-assignment.md) — the table everything above derives from.
- [Coding tools](coding-tools.md) — Annex E tools, Expert + E-AC-3 only.
- [Metadata](metadata.md) — the rest of the loudness/downmix picture, Expert only.
- [Objects & motion](objects-and-motion.md) — turning this same bed into an Atmos carrier.
