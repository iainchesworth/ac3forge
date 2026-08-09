# Live capture & session

## Starting a capture

The **Live capture** card on the left rail (see
[Loading a source](loading-a-source.md#live-capture)) is where a live session starts: pick a
device, optionally tick **Monitor** (with its own output device) and **Also write to disk**, then
**Start live session…**. This needs the platform's capture backend — see
[Platform notes](../platforms/windows.md) for what's hardware-confirmed where.

!!! note "No screenshot of an active session here"
    Every other screenshot in this guide is real capture, taken against a running build. This one
    isn't — driving an actual capture device would mean recording live audio through this
    machine's microphone just to illustrate a UI state, so this section is written from
    `src/gui/qml/Main.qml`'s Live session tab structure and `src/cli/main.cpp`'s equivalent `live`
    command instead of a screenshot. If that's ever worth revisiting, someone running the app
    locally can drop a real screenshot into `docs/gui/screenshots/` and this note can go.

## The Live session tab

Once a session is running, a **Live session** tab appears (it doesn't exist in the tab bar
otherwise) with:

- A reconnection banner if the capture device drops.
- A transport row: Stop session, a running indicator, frame count, dropped-frame count.
- A "chain" strip showing the three legs a live session can run: **Capture → Live encode →
  Receiver** (monitor and/or IEC 61937 passthrough), each leg's status visible at a glance.
- A gap banner when the receiver leg is carrying less audio than the full encode (e.g. passthrough
  refusing an E-AC-3 stream the monitor leg is still playing fine).
- A draggable **Live room** plan — the same object-placement view as
  [Objects & motion](objects-and-motion.md), active only when the session is in Atmos mode — with
  a latency readout.
- Current-layout and receiver-report cards (format/lock/underrun status from whatever's on the
  passthrough end).

This is the GUI equivalent of `ac3cli live`: capture → encode → optional live monitor and/or
passthrough, running continuously and still writing the file `record` always has. See
[CLI → Commands](../cli/commands.md#live-hardware) for the command-line form, including the
`live mode` distinction between `channels` (stereo straight through) and `atmos` (every captured
channel becomes its own moving object) — the GUI's Atmos-mode live room is that same `atmos` mode,
with the timeline replaced by real-time motion.

## Next

That's the whole app. Back to [Concepts](../concepts/index.md) for the standards this all
implements, or [Library](../library/index.md) to build something with `ac3::forge` directly.
