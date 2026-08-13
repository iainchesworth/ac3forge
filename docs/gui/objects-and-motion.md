# Objects & motion

The Objects tab is always present in Advanced and Expert, and reachable from Guided too — step 4
(**Movement**) drives the same switch, and its trajectory presets author real keyframes onto the
same objects (see below). The tab starts with a single **Encode as Dolby Atmos objects** switch;
its tab-bar entry carries an `on` badge while it's active. Off, it's out of the way entirely. On,
it takes over the format choice:

![Objects mode on: room plan and elevation, object list, motion timeline](screenshots/objects-tab.png)

Turning it on fixes the codec and layout — objects always ride as JOC + OAMD side data over a
plain 5.1 E-AC-3 bed, so the Format tab's codec field reads *Codec — fixed by object mode*, the
bed picker freezes, and the plan strip reads `E-AC-3 · 5.1 bed + <n> objects · … · .ec3`. If the
bit rate is under 384 kbps, a warning chip appears (`Objects over a 5.1 bed want 384 kbps or
better` — the metadata competes with the audio for the same frame) with a one-click **Set it**
fix, right here rather than on the tab the bit-rate control lives on.

If any of "5.1 bed", "JOC", or "OAMD" aren't already clear, read
[Concepts → Atmos & JOC](../concepts/atmos-joc.md) first — this page assumes that vocabulary.

## Objects come from the assignments

**Which channels ride as objects follows the
[assignment table](source-assignment.md#assigning-channels).** With nothing explicitly assigned,
every loaded channel becomes an object — the sensible default for a file full of stems. Once
anything is explicit, the table is the whole truth:

- A channel assigned **A new object** is a dynamic object, placed and moved in the room.
- A channel assigned to a **bed position** becomes a *static object pinned at that speaker's
  position* — in a JOC stream the bed *is* the panned objects, so "carried as a channel" and "an
  object that never leaves the L speaker" are the same coded thing. The LFE position pins as a
  pure LFE send, since no direction points at it.
- A channel assigned **Nothing** is dropped, with a named warning until that's explicit.

Dynamic objects plus pinned channels together must fit TS 103 420's sixteen-object programme cap
(the bed's LFE is one of the sixteen); an encode over it is refused with the count. An empty
object list (object mode on, nothing assigned to an object) says so — *"Objects come from the
assignments — send a sound to 'an object' and it appears here with a place in the room"* — with
an **Open assignments** button; **Add an object** and **Change what feeds them →** on the tab
itself jump to the same table.

## Sounds available, room plan, elevation, object list

- **Sounds available** (top): one chip per loaded source (`orbit51.wav · 6 ch · in use`) with
  **Import audio…**, **Add live input** (switches the rail to Live capture — one input at a time
  today) and a **Change →** jump to the assignment table that decides what each sound does.
- **Room — plan** (left): a top-down grid, front/rear. Drag anywhere to place the selected
  object — or drag the marker itself. If the object has an authored path, a note under the room
  says the drag edits its *idle* position, not the path.
- **Room — elevation** (beneath it): a true side view — the horizontal axis is the room's
  *depth* (`front … rear`), so dragging edits y and z, never x. `ceiling`, `ear level` (at the
  mockup's 66%) and the floor are marked, the bed's speakers sit on their lines for context, the
  selected object carries an `obj n · z 0.NN` chip, and its drop line reaches the floor — height
  reads as height above the ground. Height changes the *metadata*, not the bed: a 5.1 ring has
  no speakers above it, so two objects at one azimuth and different heights are identical in the
  downmix and only the object layer tells them apart. X/Y/Z readouts sit below, and they follow
  the **path** during preview rather than freezing on the idle position.
- **Objects** (right): one row per object — number, **Sound** (which loaded channel it is: `Ch
  <n>` with one source, `<file> ch <n>` with several), X/Y/Z, path (`static`, the preset's own
  name like `orbit`, or `<n> keys` for a hand-authored one), LFE send, and keyframe count. The
  count line keeps the budget honest — the denominator is what is genuinely left once bed-pinned
  channels have spent their slots (`4 of 15 objects · 2 pinned to the bed`), since the bed's LFE
  is the sixteenth. The selected object gets an **LFE send** slider (0.00–1.00) — the only route
  to that channel, since panning never reaches it.

## Motion

A timeline beneath the object list — a ruler over eight seconds, one lane per object with its
keyframes as rotated squares, and a playhead. It is an *editor*, not a display:

- **Click or drag** anywhere on the timeline to scrub the playhead (pausing a running preview).
- **Double-click a lane** to author a key at that instant from the object's current position.
- **Drag a key** to retime it — the move commits on release, and landing on another key replaces
  it (one instant, one cue).
- **Right-click a key** (or select it and press **Delete key**) to remove it.
- **Add key** captures the selected object's current position at the playhead. A hand-added key
  seeds the same `0.7/√n` gain the path-less fallback encodes at, so an object never jumps
  louder the moment its first cue lands.
- **Preview** plays every path back in the plan view *and* the elevation view, moving the
  markers along exactly what `encodeObjects` will place (the same `KeyframePath` evaluation, not
  a second interpolation that could disagree with it).

Guided step 4 offers **trajectory presets** — *Stay put*, *Circle the room* (one lap around the
listener over eight seconds, objects spaced apart), *Lift overhead* (floor to ceiling and back),
and *Place them myself*, which is this tab — that author real keyframes through the same API, so
a preset is a starting point on this timeline, not a separate motion system. A path a preset
authored keeps the preset's name in the object table until a hand edit makes it something else.

**Author a path / Drive it live** (top right) are the two ways to get motion in. Live driving
needs a monitored capture — the option points at [Live session](live-session.md) rather than
offering a dead control; during a live Atmos session the room is dragged in real time instead of
keyframed.

See [Spatial & Atmos objects](../library/spatial-and-atmos.md) for the `ac3::oba::motion` API
this timeline is a UI over — `Keyframe`, `KeyframePath`, and `evaluate_placements`.

## Next

[Live capture & session](live-session.md) — the same object machinery, but driven from a live
capture instead of a file.
