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
plain file encoding — nothing there knows whether the *next* thing clicked is Encode or Start
live session, so a note appears on the Input block's live branch instead, whenever VBR is on and
available:

![VBR panel with the live-session warning visible in the rail's live branch](screenshots/format-vbr.png)

A live session always runs at the fixed bit rate, regardless of the rate mode: IEC 61937
passthrough bursts are fixed-size per access unit, and nothing renegotiates burst framing
mid-stream, so `runLiveSession` drops `vbr` unconditionally before a session ever starts. There's
also no "finished run" for a live session to summarize a variable rate against the way a file
encode's run strip does.

!!! note "No screenshot of an active session here"
    Every other screenshot in this guide is real capture, taken against a running build. This one
    isn't — driving an actual capture device would mean recording live audio through this
    machine's microphone just to illustrate a UI state, so this section is written from
    `src/gui/qml/Main.qml`'s Live session tab structure and `src/cli/main.cpp`'s equivalent
    `live` command instead of a screenshot. If that's ever worth revisiting, someone running the
    app locally can drop a real screenshot into `docs/gui/screenshots/` and this note can go.

## The Live session tab

Once a session is running, a **Live session** tab appears (it doesn't exist in the tab bar
otherwise) with:

- A reconnection banner while the receiver re-locks to a new bitstream format — a layout change
  is a deliberate, visible act, and about a second of audio is lost; the banner says so rather
  than hiding the dropout.
- A transport row: Stop session, a running indicator, frame count, dropped-frame count, and
  whether the take is being written to disk.
- A "chain" strip showing the three legs as separate plans: **Capture** (the actual device) →
  **Live encode** (follows the picker — what the meters and soundfield show) → **Receiver leg —
  IEC 61937** (capped at what can be bitstreamed today).
- A gap banner whenever the receiver leg carries less than the encode — everything past it, and
  every object move, is visible on the meters and the soundfield but not audible on the
  amplifier until DD+ passthrough lands.
- A draggable **Live room** plan — the same object-placement view as
  [Objects & motion](objects-and-motion.md), active only in Atmos mode, applying each drag to the
  running encode immediately — with a latency readout.
- A **Layout** switcher and a receiver-reports card (see below).

## Switching layout mid-session

The **Layout — switching re-locks the receiver** card offers the presets (5.1, 7.1, 5.1.4,
7.1.4). Picking one *stops the running session, applies the preset, and starts a new session with
the same capture/monitor/receiver choices* — the deliberate stop-renegotiate-resume the
reconnection banner narrates, not a silent switch. Layouts past 5.1 carry an accent dot and a
legend: they encode and meter, but the receiver leg stays Dolby Digital 5.1 until DD+ passthrough
lands. The switcher refuses two states honestly: object mode (the layout is fixed at a 5.1 bed —
the card says so) and a take being written to disk (a restart would clobber the first half of the
file; stop the session and start a new take instead).

This is the GUI equivalent of `ac3cli live`: capture → encode → optional live monitor and/or
passthrough, running continuously and still writing the file `record` always has. See
[CLI → Commands](../cli/commands.md#live-hardware) for the command-line form, including the
`live mode` distinction between `channels` and `atmos` — the GUI's Atmos-mode live room is that
same `atmos` mode, with the timeline replaced by real-time motion.

## Next

That's the whole app. Back to [Concepts](../concepts/index.md) for the standards this all
implements, or [Library](../library/index.md) to build something with `ac3::forge` directly.
