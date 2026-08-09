# ac3gui — window layout

`ac3gui` (window title `ac3forge`, QML module `Ac3Forge`) is a Qt Quick front end over the same
`ac3::forge` library documented under [Library](../library/index.md) — nothing in the GUI has
logic the library doesn't also expose, and every setting it makes maps onto an equivalent
[`ac3cli`](../cli/index.md) invocation shown live at the bottom of the window.

The screenshots in this guide are of the current two-pane "workbench" layout. An earlier
nine-card single-column design existed before it — if you find references to that layout
elsewhere in the repo's history, they describe a superseded build; see the
[superseded design brief](../project/gui-design-brief.md) if you want the history.

## The window

Minimum size 1280×900. Two panes, divided by a vertical rule:

![Default window state, no source loaded, Basic mode](screenshots/overview-default.png)

- **Header** (top): the `ac3forge` wordmark and subtitle, a **Basic / Advanced** segmented
  control, and a **Preferences** button.
- **Left rail — "the signal"** (always visible, never scrolled away): three cards —
  [Source](loading-a-source.md#source), [Live capture](loading-a-source.md#live-capture), and
  [Channel levels](loading-a-source.md#channel-levels). This is what's coming *in*.
- **Right panel — "the stream"** (tabbed): a plan strip showing the derived output headline
  (`<codec> · <shape> · <bitrate> kbps · .<suffix>`) and the equivalent Annex E tools token, then
  tabs for [Format](format-and-channels.md), [Coding tools](coding-tools.md) and
  [Metadata](metadata.md) (the latter two, Advanced mode only), [Objects](objects-and-motion.md),
  and [Live session](live-session.md) (only while a live session is running). This is what's
  going *out*.
- **Run strip** (bottom): past and in-flight encode runs, a live-generated `ac3cli` command line
  with a Copy button, and the primary Encode button.

## Basic vs. Advanced

**Basic** shows just enough to encode a file at a sensible default: Format tab only, with a
Loudness card folded into it. **Advanced** adds the Coding tools and Metadata tabs, plus a
Passthrough-to-a-receiver card on the Format tab, for the Annex E tools and broadcast metadata
that most encodes don't need to touch. Switching modes doesn't discard anything you've already
set — it only changes what's visible.

## Next

Walk the panes in the order a first encode actually goes:

1. [Loading a source](loading-a-source.md) — pick a WAV, or capture live; watch the channel meters
2. [Format & channels](format-and-channels.md) — codec, layout, bit rate, container
3. [Coding tools](coding-tools.md) — Annex E tools (Advanced, E-AC-3 only)
4. [Metadata](metadata.md) — loudness, downmix, heavy compression (Advanced)
5. [Objects & motion](objects-and-motion.md) — Dolby Atmos objects
6. [Live capture & session](live-session.md) — capture → encode → monitor/passthrough, live

Or start with [Concepts](../concepts/index.md) if terms like "dependent substream" or "JOC" are
unfamiliar — the GUI uses the same vocabulary as the standards it implements.
