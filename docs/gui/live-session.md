# Live capture & session

## Starting a capture

The rail's [Input block](loading-a-source.md#01--input), switched to **Live capture**, is where a
device gets chosen and where the two signal-side acts live: **Monitor** (a session that writes
nothing and asks for no filename — the meters and soundfield run against the real encoded signal,
and checking a device never commits to a take) and **Record…** (capture straight to a file). That
is all the rail keeps now. Where a *real* session — one with a take on disk or a receiver leg —
actually starts is the **Live session** tab's own **Live session** Card.

The Card has two states. Idle, it is the pre-flight: a **Receiver** combo (`No passthrough` plus
every entry from `EncoderController.outputDevices`), a **Monitor** checkbox (checked by default),
an **Also write the take to disk** checkbox, a **Raw-WAV safety copy** checkbox — enabled only
once write-to-disk is checked, see [Take durability](#take-durability) below — and a highlighted
**Start session** button, enabled whenever `EncoderController.captureSupported &&
!EncoderController.busy`. Checking write-to-disk turns Start session into a save-file prompt
first; the chosen path is what `EncoderController.startLiveSession(captureDeviceIndex, monitor,
receiverDeviceIndex, writeToDisk, fileUrl)` receives once the dialog closes — leave it unchecked
and the same invokable runs immediately with an empty path instead. Running, the Card becomes the
transport: a **Stop session** button, zero-padded RUNNING / FRAMES / DROPPED counters, and a
disabled **Also writing the take to disk** readout, since that choice was made pre-flight and
cannot change mid-session.

![The idle Live session card — receiver, monitor, write-to-disk and the highlighted Start session
button](screenshots/live-session-idle.png)

The auto-focus rule is unchanged by the move: starting a *real* session — a take on disk or a
receiver leg wanted (`EncoderController.liveWritingToDisk ||
EncoderController.liveWantedPassthrough`) — still focuses the tab; Monitor, however it was
started, never steals the tab you were configuring.

### The VBR warning

[Rate mode](format-and-channels.md#rate-mode-cbr-or-vbr) lives on the Format tab, shared with
plain file encoding — and, per the design, it does not render at all while the live source is
selected: a live session cannot honour it, so the control would only mislead. A VBR choice made
earlier still exists, so a note now appears on the **Live session** tab's own Card, in its idle
state, whenever VBR is on (`objectName: "liveVbrWarning"`, bound to `EncoderController.vbrEnabled
&& EncoderController.vbrAvailable`) — not on the rail any more, now that the rail's live branch
keeps only the signal-side acts:

![The live-session VBR warning, now on the Live session tab's own
card](screenshots/live-session-vbr-note.png)

A live session always runs at the fixed bit rate, regardless of the rate mode: IEC 61937
passthrough bursts are fixed-size per access unit, and nothing renegotiates burst framing
mid-stream, so `runLiveSession` drops `vbr` unconditionally before a session ever starts. The
run entry a real session opens says so too — its rate text is always the fixed rate.

!!! note "No screenshot of an active session here"
    The idle Card above is real capture — taken against a running build, with a genuine device
    enumerated on this machine. A session actually *running* is not: driving one would mean
    recording live audio through this machine's microphone just to illustrate a UI state, so the
    running transport, the chain strip mid-run, the reconnection banner, and the Live room's
    live-only controls are written from `src/gui/qml/Main.qml`'s Live session tab structure and
    `src/cli/main.cpp`'s equivalent `live` command instead of a screenshot. If that's ever worth
    revisiting, someone running the app locally can drop a real screenshot into
    `docs/gui/screenshots/` and this note can go.

## The Live session tab

The **Live session** tab exists whenever the live source is selected in the rail (Advanced and
Expert) — it is where a session is *understood*, not a modal that only appears once one is
already underway. It carries:

- A reconnection banner while the receiver re-locks to a new bitstream format — named after the
  actual endpoint (*"Renegotiating with Denon AVR-X3800H."*), because the session knows exactly
  who is re-locking. This fires for a session's own first passthrough open and for a
  [receiver hot-swap](#receiver-hot-swap) alike — either way about a second of audio is lost, and
  the banner says so rather than hiding the dropout; a **Skip** dismisses it early for whoever can
  hear the receiver has already settled.
- The transport row described above: Stop session, RUNNING/FRAMES/DROPPED, and the disabled
  write-to-disk readout.
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
  in this session** chip list. A live Atmos session pre-allocates a fixed *budget* of object slots
  at start, `clamp(max(8, device.channels), 8, 15)`, baked into the `ac3::oba::AtmosEncoder`'s
  construction in `runLiveSession` and unable to change mid-session — that is how JOC's own object
  count works — rather than the device's channel count exactly: a two-channel device still gets
  eight slots to grow into, a device with more than eight starts with all of them bound identity-
  wise (slot *i* fed by capture channel *i*). Which capture channel feeds which slot is otherwise
  live and mutable: a channel-picker ComboBox — its "Ch 1"…"Ch N" entries built off
  `EncoderController.liveDeviceChannels`, the live device's own channel count (0 outside a
  session) — plus **Add** (`EncoderController.addLiveObject(captureChannel)`, binds the next free
  slot), **Reassign selected** (`EncoderController.reassignLiveObjectSlot(slotIndex,
  captureChannel)`, acting on whichever object the room has selected) and **Silence selected** (the
  same call with a negative channel) sit on the Card, visible only while live. Every slot's current
  binding (or −1 for unbound) is also published as `EncoderController.liveObjectChannels`, index-
  aligned with the object model, for anything else that wants to read it back. The **OBJECTS IN
  THIS SESSION** counter reads `N of M slots live` (bound slots over the budget —
  `EncoderController.liveObjectSlotsBound` over `objectCount`) while a session is running, falling
  back to the earlier `N objects live` wording for the non-live, file-loaded-object-mode case.
  Beside the room, an x/y/z/latency readout grid tracks whichever object is selected; latency
  starts as `EncoderController.liveLatencyMs`'s two-frame estimate (one period to fill the capture
  buffer, one to encode and hand off) and, once monitoring has run for about a second and the
  pipeline's startup transients have passed, is replaced by the real measured capture-to-monitor
  round trip. `EncoderController.liveLatencyMeasured` says which is current — the label reads
  `~N ms measured` once that lands, `~N ms est.` until then or whenever monitoring is off, since
  there is nothing to time a round trip against.
- A **Layout** switcher and a receiver-reports card (see below). The same **Receiver** combo above
  the Card serves both phases of a session: before Start it is the pre-flight pick; once live, an
  explicit choice hot-swaps the passthrough leg instead of restarting anything — see
  [Receiver hot-swap](#receiver-hot-swap). The reports card leads with the receiver-display rows
  the mockup draws — **Format** (`DOLBY DIGITAL PLUS`) and **Input** (the leg's shape) — above
  Lock, Underruns and Monitor.

Real sessions also land in the [run history](format-and-channels.md): a take or a receiver leg
opens a run entry (duration `live`), so a mid-session failure — including the device-drop
watchdog's own, see [Device-drop detection](#device-drop-detection) — has a chip and a banner to
land on, and a finished take has a **Show in folder**. Monitor-only checks deliberately stay out
of the history, watchdog failures included: only a take on disk or a receiver leg opens the entry
in the first place, and that was already true before this bundle — the watchdog just inherits it.

## Take durability

`runLiveSession` used to accumulate every encoded unit in memory and write the whole take once at
the end — an hour-long session was unbounded memory, and a crash lost everything captured. It now
writes each encoded unit to disk as it is produced.

The output file (or files) opens before the session is marked live: a new private
`openLiveOutputWriters` builds a `LiveOutputWriters` — the open `stream`, its `stream_path`, the
real `final_path`, a `matroska` flag, a `frame_sizes` index, and an optional `wav_safety` writer —
on the GUI thread, inside `startLiveSession`, so a bad destination path is refused there exactly
like a bad device choice already is, not discovered as a mid-take failure minutes in.

What "writing incrementally" means depends on the container:

- **Elementary stream** (`.ac3` / `.ec3`): `stream_path` and `final_path` are the same file. Every
  byte `runLiveSession`'s loop writes *is* the take, from the first frame on — a crash leaves
  exactly what was captured, playable up to that point.
- **Matroska** (`.mkv`): `matroska::mux()` only ever produces a complete file from the whole set of
  frames at once, so the real `.mkv` can only be written once, at a clean stop. The elementary
  stream instead spools to a companion file next to the real path — same stem, extension replaced
  with `.live.ec3` or `.live.ac3` depending on codec (`live_stream_spool_path`, in
  `encoder_controller.cpp`) — while `frame_sizes` tracks a lightweight per-frame size index, not
  the audio itself, so memory stays bounded for the run's whole duration. At a clean stop the spool
  is read back, muxed into the real `.mkv`, and deleted. A crash (or any interruption that never
  reaches the mux step) leaves the `.live.ec3` / `.live.ac3` spool behind as the recoverable
  elementary take — worth knowing if a Matroska session ever ends badly: the audio survives next to
  where the `.mkv` would have been, just not yet in a Matroska container.

Both paths flush to disk roughly once a second (not per frame) rather than on every write.

There is also an optional **raw-WAV safety copy**: the pre-flight "Raw-WAV safety copy" checkbox,
bound to `EncoderController.liveWavSafetyCopy`, is only consulted once the take is also being
written to disk. When on, it streams the raw captured PCM — device channel order, unencoded,
before any routing or mixing — to a sibling `.raw.wav` file via `ac3::io::WavStreamWriter`
(`src/lib/include/ac3/io/wav.hpp`), a writer built for exactly this: it appends interleaved samples
as they arrive and, like the take itself, periodically re-patches its RIFF header
(`flush_header()`) rather than only at close. Without that, a process kill mid-session leaves a WAV
whose header still claims zero data bytes even though the file holds real audio — most readers
trust the header's declared size over the file's actual length, so an unpatched header would make a
real partial take *look* empty. Patching it every second or so means the worst a hard crash can do
is undersell the last fraction of a second.

## Device-drop detection

The capture read loop now runs an `ac3::capture::SilenceWatchdog`
(`src/audio/include/ac3/capture/watchdog.hpp`), reset once when the loop starts and fed after every
read attempt via `on_read(got, now)` — only whether `got` is zero matters. `timed_out(now)`
answers whether the last non-empty read is more than the watchdog's timeout ago; `runLiveSession`
constructs it with a three-second timeout (`kDeviceSilenceTimeout` in `encoder_controller.cpp`).

Once that gap passes, the session stops as a failure, not a silent "still running": the status
line and the failure banner name the device —

> "Microphone (Logitech StreamCam)" stopped delivering audio - the capture device may have been
> disconnected. Wrote 212 frames before it went quiet.

— and the word "device" in that text is what already makes the existing failure banner's **Choose
another device** action appear (`failureBanner.deviceClass` in `Main.qml` looks for exactly that
word); the banner infrastructure already existed, this bundle just gives it a real trigger. For a
real session, the run entry lands with status `failed` and its frame count, read back out of the
same message rather than counted a second time. A monitor-only session that loses its device still
stops and the status line still updates the same way — it just has no run chip or banner to land
on, because monitor-only checks never open a run entry in the first place (true before this bundle
too; the watchdog does not change it, and is worth being honest about rather than overselling).

`tests/test_watchdog.cpp` is a new Catch2 unit test covering the watchdog's own timing logic in
isolation — no GUI, no real device, just the clock the caller drives explicitly.

## Receiver hot-swap

Changing the **Receiver** combo on the Live session tab's own Card — or picking `No passthrough` —
while a session is running no longer requires a stop.
`EncoderController.switchLiveReceiver(receiverDeviceIndex)` (a negative index turns passthrough
off) closes whatever passthrough sink is open and opens a new one for the chosen endpoint,
entirely on `runLiveSession`'s own worker thread, between frames: the request is handed off
through a small guarded slot the worker claims once per frame period, on the one thread that ever
calls `submit()` on the sink, so there is never a window where two threads touch it at once.
Capture and encode keep running uninterrupted through the swap — only the receiver leg blinks.

A hot-swap that cannot open (the chosen endpoint refuses the current format) shows the same
refusal text a fresh session's own first passthrough open already uses — both call the same
`open_live_passthrough` helper rather than keeping two copies of the open-and-explain logic. The
existing reconnection banner ("Renegotiating with X… expect a second of silence") fires exactly as
it does for a session's initial passthrough open: a hot-swap is a real exclusive-mode re-open too,
so the same brief interruption applies.

On the Live session tab, the same receiver ComboBox serves both roles — before Start it is the
pre-flight choice `startLiveSession` reads; once live, an explicit pick (never a binding) calls
`switchLiveReceiver` instead.

This is a different act from **switching layout**, below: that still stops the whole session,
applies the preset, and starts a fresh one; a hot-swap never stops anything, and only ever
changes the receiver leg.

## Switching layout mid-session

The **Layout — switching re-locks the receiver** card offers the presets (5.1, 7.1, 5.1.4,
7.1.4) — and the Format tab's own preset buttons do the same thing during a live session, per
the design's interaction table. Picking one *stops the running session, applies the preset, and
starts a new session with the same capture/monitor/receiver choices* — the deliberate
stop-renegotiate-resume the reconnection banner narrates, not a silent switch, and not the lighter
[receiver hot-swap](#receiver-hot-swap) above, which never stops anything at all. The dots and the
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
same `atmos` mode, with the timeline replaced by real-time motion. The parity has a real gap
today, worth being honest about: `ac3cli live` (`run_live` in `src/cli/main.cpp`) still reserves
and fills a `frames` vector across the whole run and writes it once at the end, has no device-drop
watchdog, and still pans exactly one object per capture channel with no add/reassign — none of
this page's [take durability](#take-durability), [device-drop detection](#device-drop-detection),
live object-slot budget, or [receiver hot-swap](#receiver-hot-swap) has reached the CLI's `live`
command yet.

## Next

That's the whole app. Back to [Concepts](../concepts/index.md) for the standards this all
implements, or [Library](../library/index.md) to build something with `ac3::forge` directly.
