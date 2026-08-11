# Objects & motion

The Objects tab is always present in Advanced and Expert (Guided keeps object placement and the
multi-source assignment table out of its own step sequence — both are inherently non-linear, which
a five-step wizard has nowhere honest to put; switch to Advanced or Expert to reach them), and
starts with a single **Encode as objects** switch. Off, it's out of the way entirely. On, it takes
over the format choice:

![Objects mode on: 5.1 bed, JOC + OAMD, room plan, object list, motion timeline](screenshots/objects-tab.png)

Turning it on fixes the codec and layout — objects always ride as JOC + OAMD side data over a
plain 5.1 E-AC-3 bed, so the Format tab's codec/layout controls go read-only and the plan strip
grows a third line: `5.1 bed · JOC + OAMD · objects carry the height`. If the loaded source has
more channels than the current object count covers, a warning appears (`the bed is 5.1 — 384
kbps or more`) with a one-click **Set it** fix.

If any of "5.1 bed", "JOC", or "OAMD" aren't already clear, read
[Concepts → Atmos & JOC](../concepts/atmos-joc.md) first — this page assumes that vocabulary.

## Room plan and object list

**Every source channel becomes an object**, panned into the 5.1 bed that any ordinary decoder can
still play; the object *positions* ride alongside as metadata, so a height is carried even though
no bed channel can reproduce it, and the LFE send is the only route to that channel (no direction
point aims at it).

- **Room — plan** (left): a top-down grid, front/rear, X/Z on the floor. Drag a marker to place an
  object; the current selection's X/Y/Z coordinates show beneath the grid.
- **Objects** (right): one row per object — source channel, X/Y/Z, an assigned motion path, LFE
  send, and keyframe count. With exactly one source loaded, the **Source** column reads `Ch <n>`;
  with more than one (see [Multi-source & assignment](source-assignment.md)), it reads
  `<file> ch <n>` instead, so an object's row always names which loaded file it actually came
  from — an object is addressed the same flat (source, channel) way `map=`/the assignment table
  address a channel, just carried through into object mode instead of being routed onto a bed.
  Each selected object gets a **Height** slider (Y axis — height changes the metadata, not the bed
  itself, since a 5.1 ring has no speakers above it) and an **LFE send** slider.

## Motion

A timeline beneath the object list, scrubbing from `0:00.00` to the source's duration. **Add key**
drops a keyframe at the current position for the selected object; **Preview** plays the path back
in the plan view. **Author a path** and **Drive it live** (top right of the card) are the two ways
to get motion in: an authored keyframe file (backing `ac3cli atmos-path`) or a live-driven source
(the hook `ac3cli live`'s `atmos` mode uses, moving each captured channel's object placement every
frame from elapsed time).

See [Spatial & Atmos objects](../library/spatial-and-atmos.md) for the `ac3::oba::motion` API this
timeline is a UI over — `Keyframe`, `KeyframePath`, `OrbitPath`, and `evaluate_placements`.

## Next

[Live capture & session](live-session.md) — the same object machinery, but driven from a live
capture instead of a file.
