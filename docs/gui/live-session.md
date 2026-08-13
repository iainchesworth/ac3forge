# Live capture & session

## Starting a capture

The rail's [Input block](loading-a-source.md#01--input), switched to **Live capture**, is where a
live session starts: pick a device, then either **Monitor** (a session that writes nothing and
asks for no filename — the meters and soundfield run against the real encoded signal, and
checking a device never commits to a take), **Record…** (capture to a file), or **Start live
session…** with its Monitor / passthrough-receiver / **Also write the take to disk** options.
This needs the platform's capture backend — see [Platform notes](../platforms/windows.md) for
what's hardware-confirmed where.

### The VBR warning

[Rate mode](format-and-channels.md#rate-mode-cbr-or-vbr) lives on the Format tab, shared with
plain file encoding — and, per the design, it does not render at all while the live source is
selected: a live session cannot honour it, so the control would only mislead. A VBR choice made
earlier still exists, so a note appears on the Input block's live branch whenever it is on,
naming the rule; the Format tab's Bit rate field meanwhile relabels itself as the band-edge
reference it becomes under VBR:

![The live-session VBR warning visible in the rail's live branch](screenshots/format-vbr.png)

A live session always runs at the fixed bit rate, regardless of the rate mode: IEC 61937
passthrough bursts are fixed-size per access unit, and nothing renegotiates burst framing
mid-stream, so `runLiveSession` drops `vbr` unconditionally before a session ever starts. The
run entry a real session opens says so too — its rate text is always the fixed rate.

!!! note "No screenshot of an active session here"
    Every other screenshot in this guide is real capture, taken against a running build. This one
    isn't — driving an actual capture device would mean recording live audio through this
    machine's microphone just to illustrate a UI state, so this section is written from
    `src/gui/qml/Main.qml`'s Live session tab structure and `src/cli/main.cpp`'s equivalent
    `live` command instead of a screenshot. If that's ever worth revisiting, someone running the
    app locally can drop a real screenshot into `docs/gui/screenshots/` and this note can go.

## The Live session tab

The **Live session** tab exists whenever the live source is selected in the rail (Advanced and
Expert) — it is where a session is *understood*, not a modal that only appears once one is
already underway. Starting a real session (a take on disk, or a receiver leg) focuses it;
merely monitoring never steals the tab you were configuring on. It carries:

- A reconnection banner while the receiver re-locks to a new bitstream format — named after the
  actual endpoint (*"Renegotiating with Denon AVR-X3800H."*), because the session knows exactly
  who is re-locking. A layout change is a deliberate, visible act, and about a second of audio
  is lost; the banner says so rather than hiding the dropout, and a **Skip** dismisses it early
  for whoever can hear the receiver has already settled.
- A transport row: Stop session, a zero-padded running clock, space-grouped frame and
  dropped-frame counts, and whether the take is being written to disk.
- A "chain" strip showing the three legs as separate plans: **Capture** (the actual device, with
  its `2 ch · 48 000 Hz` sub-line) → **Live encode** (follows the picker — what the meters and
  soundfield show, printed without a file suffix a session may never write) → **Receiver leg —
  IEC 61937** (with the burst data type it is actually sending).
- A gap banner when the receiver leg carries less than the encode — today that is exactly object
  mode, whose leg is the 5.1 bed: every object move is visible on the meters and the soundfield,
  but a consumer decoder gates object decoding, so the amplifier plays the bed, not the motion.
  A passthrough that was asked for and did *not open* gets its own banner instead, carrying the
  reason — "everything past what the leg carries" would be a lie when the leg carries nothing.
- A draggable **Live room** plan — the same object-placement view as
  [Objects & motion](objects-and-motion.md) with its crosshair and wall names, active only in
  Atmos mode, applying each drag to the running encode immediately — plus a read-only **Objects
  in this session** chip list with its live counter, and an x/y/z/latency readout grid (the
  latency honestly labelled an estimate).
- A **Layout** switcher and a receiver-reports card (see below). The reports card leads with the
  receiver-display rows the mockup draws — **Format** (`DOLBY DIGITAL PLUS`) and **Input**
  (the leg's shape) — above Lock, Underruns and Monitor.

Real sessions also land in the [run history](format-and-channels.md): a take or a receiver leg
opens a run entry (duration `live`), so a mid-session failure has a chip and a banner to land
on, and a finished take has a **Show in folder**. Monitor-only checks deliberately stay out of
the history.

## Switching layout mid-session

The **Layout — switching re-locks the receiver** card offers the presets (5.1, 7.1, 5.1.4,
7.1.4) — and the Format tab's own preset buttons do the same thing during a live session, per
the design's interaction table. Picking one *stops the running session, applies the preset, and
starts a new session with the same capture/monitor/receiver choices* — the deliberate
stop-renegotiate-resume the reconnection banner narrates, not a silent switch. The dots and the
legend are derived from the **actual receiver**: against an AC-3-only endpoint, layouts past 5.1
carry the dot and the legend names the device (*"…bitstreams Dolby Digital only — its leg stays
5.1"*); an E-AC-3-capable receiver bitstreams every layout as encoded, and the legend says that
instead. The switcher refuses two states honestly: object mode (the layout is fixed at a 5.1
bed — the card says so) and a take being written to disk (a restart would clobber the first half
of the file; stop the session and start a new take instead).

This is the GUI equivalent of `ac3cli live`: capture → encode → optional live monitor and/or
passthrough, running continuously and still writing the file `record` always has. See
[CLI → Commands](../cli/commands.md#live-hardware) for the command-line form, including the
`live mode` distinction between `channels` and `atmos` — the GUI's Atmos-mode live room is that
same `atmos` mode, with the timeline replaced by real-time motion.

## Next

That's the whole app. Back to [Concepts](../concepts/index.md) for the standards this all
implements, or [Library](../library/index.md) to build something with `ac3::forge` directly.
