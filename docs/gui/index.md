# ac3gui — window layout

`ac3gui` (window title `ac3forge`, QML module `Ac3Forge`) is a Qt Quick front end over the same
`ac3::forge` library documented under [Library](../library/index.md) — nothing in the GUI has
logic the library doesn't also expose, and every setting it makes maps onto an equivalent
[`ac3cli`](../cli/index.md) invocation shown live at the bottom of the window.

The screenshots in this guide are of the current two-pane "workbench" layout. An earlier
nine-card single-column design existed before it, and a two-tier Basic/Advanced control before
that — if you find references to either elsewhere in the repo's history, they describe a
superseded build predating this guide.

## The window

Minimum size 1280×900. Two panes, divided by a vertical rule:

![Default window state, no source loaded, Advanced tier](screenshots/overview-default.png)

- **Header** (top): the `ac3forge` wordmark and subtitle, a **Guided / Advanced / Expert**
  segmented control, and a **Preferences** button.
- **Left rail — "the signal"** (always visible, never scrolled away, and never affected by which
  tier is selected): three cards — [Source](loading-a-source.md#source),
  [Live capture](loading-a-source.md#live-capture), and
  [Channel levels](loading-a-source.md#channel-levels). This is what's coming *in*.
- **Right panel — "the stream"**: a plan strip showing the derived output headline
  (`<codec> · <shape> · <bitrate> kbps` or, in VBR mode, `<codec> · <shape> · quality <n>` —
  `· .<suffix>`) and the equivalent Annex E tools token. What fills the rest of the panel depends
  on the tier — see below.
- **Run strip** (bottom): past and in-flight encode runs, a live-generated `ac3cli` command line
  with a Copy button, and the primary Encode button. Present in every tier, including Guided.

## Guided, Advanced, Expert

Three tiers, not two — Guided is new; Advanced and Expert are renames of what this guide used to
call Basic and Advanced, one notch further apart than before.

- **Guided** (the default for a new session) replaces the tabbed right panel entirely with a
  five-step wizard — Source, Format, Rate mode (E-AC-3 only, skipped otherwise), Loudness, Review
  — that reads and writes the exact same state Advanced and Expert do. There is no separate
  "wizard draft": switch tiers mid-session and whatever the wizard set is exactly what Advanced or
  Expert already show for the same field, and vice versa.

  ![Guided tier's Source step, no source loaded yet](screenshots/guided-wizard-source.png)

  Loading a source, adding more of them, and live capture all still happen on the left rail —
  the wizard's own Source step just points at it rather than duplicating a file picker. Object
  (Atmos) placement and the multi-source assignment table are Advanced/Expert only: both are
  inherently non-linear (a spatial canvas, a table), which a five-step sequence has nowhere
  honest to put them.
- **Advanced** shows a tabbed right panel — [Format](format-and-channels.md) with a Loudness card
  folded in, and [Objects](objects-and-motion.md) — enough to encode a file at a sensible default
  with full control over the channel picker, without the Annex E tools or broadcast metadata most
  encodes don't need to touch.
- **Expert** adds the [Coding tools](coding-tools.md) and [Metadata](metadata.md) tabs, plus a
  Passthrough-to-a-receiver card on the Format tab in place of the Loudness one (Metadata absorbs
  loudness instead, alongside downmix). [Live session](live-session.md) joins the tab bar in any
  tier, but only while a session is actually running — it doesn't exist otherwise.

Switching tiers never discards anything already set — it only changes what's visible (and, for
Guided, how it's presented: one question at a time instead of a page of controls).

## Next

Walk the panes in the order a first encode actually goes:

1. [Loading a source](loading-a-source.md) — pick a WAV (or several — see multi-source
   below), or capture live; watch the channel meters
2. [Format & channels](format-and-channels.md) — codec, layout, dual mono, VBR, bit rate,
   container
3. [Coding tools](coding-tools.md) — Annex E tools (Expert, E-AC-3 only)
4. [Metadata](metadata.md) — loudness, downmix, heavy compression (Expert)
5. [Objects & motion](objects-and-motion.md) — Dolby Atmos objects
6. [Live capture & session](live-session.md) — capture → encode → monitor/passthrough, live

Loading more than one source at once — each channel individually assigned to a bed position, an
object, or a dual-mono programme — is its own page:
[Multi-source & assignment](source-assignment.md).

Or start with [Concepts](../concepts/index.md) if terms like "dependent substream" or "JOC" are
unfamiliar — the GUI uses the same vocabulary as the standards it implements.
