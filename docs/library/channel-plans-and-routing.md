# Channel plans & routing

`ac3::plan::LayoutId` only ever names eight hand-picked combinations (mono through 7.1.4). The
two APIs on this page are the general machinery underneath: any Table E2.5 channel selection at
all, and any number of separate sources feeding it.

## Custom channel selections: `Plan::custom_locations`

`ac3/encoder/plan.hpp` and `ac3/core/eac3_tables.hpp`. `ac3::eac3::chanmap::allocate` partitions
an arbitrary set of Table E2.5 locations into a bed (the widest Table 5.8 acmod whose own
locations all fit) and however many dependents the remainder needs. `Plan::custom_locations`
is the front door onto it: set it and it overrides `layout` entirely.

```cpp
const auto locations = ac3::plan::parse_channels("L,C,R,Ls,Rs,LFE,Ts,Lw,Rw");

const ac3::plan::Plan plan{
    .codec = ac3::plan::Codec::kEac3,
    .custom_locations = *locations,
    .bitrate_kbps = 640,
};
```

```cpp
const auto channel_plan = ac3::plan::resolve(plan);   // the bed + dependent chanmaps
const auto config = ac3::plan::eac3_config(plan);      // AccessUnitConfig, ready to encode
ac3::eac3::AccessUnitEncoder encoder{config};
```

Full program: [`examples/custom_layout.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/custom_layout.cpp)
— 5.1 plus a top-surround channel and a front-wide pair, a layout no `LayoutId` names, built and
encoded the same way a caller with an unusual speaker set would.

`parse_channels`/`format_channels` round-trip a comma-separated list of Table E2.5 names
(`ac3::eac3::chanmap::name()`'s own spelling, e.g. `"Ls"`, `"LFE2"`); a pair location (Lc/Rc,
Lrs/Rrs, Lsd/Rsd, Lw/Rw, Vhl/Vhr, Lts/Rts) must name both members, since Table E2.5 has no bit
for one alone. `allocate()` fails with `AllocationError::kNoBedFit` if no Table 5.8 acmod's own
locations are a subset of the request, or `kOrphanLfe2` if `LFE2` is asked for with no
full-bandwidth channel left to share its substream.

`ac3::plan::coded_channels(plan)`/`coded_channel_names(plan)` work over a resolved
`ChannelPlan` exactly the way they do over a named `LayoutId` — a custom selection and a named
layout go through the same reporting path.

## Multiple sources: `ac3::plan::Assignment`

`ac3/encoder/assignment.hpp`. `plan::route()`'s other overload places *one* source by
direction — the microphone-onto-5.1 case. A caller with several sources — several files,
several capture devices, or dual mono's two independent programmes — instead says exactly
where each of *their* channels goes, channel by channel.

```cpp
const std::array<ac3::plan::SourceShape, 2> sources{{
    {.channels = 2, .label = "music.wav"},
    {.channels = 1, .label = "voiceover.wav"},
}};

ac3::plan::Assignment assignment;
assignment.set(0, 0, {.kind = ac3::plan::DestinationKind::kLocation, .location = Location::kLeft});
assignment.set(0, 1, {.kind = ac3::plan::DestinationKind::kLocation, .location = Location::kRight});
// -6 dB under the music, so the voiceover reads without burying it.
assignment.set(1, 0, {.kind = ac3::plan::DestinationKind::kLocation,
                      .location = Location::kCentre,
                      .trim_db = -6.0});
```

```cpp
const auto routing = ac3::plan::route(target, sources, assignment);
ac3::plan::render(*routing, source_views, coded_views, ac3::kSamplesPerFrame);
```

Full program: [`examples/multi_source_assignment.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/multi_source_assignment.cpp)
— a stereo music bed and a separate mono voiceover, combined onto one 5.1 stream.

`Destination::kind` is a closed set: `kUnassigned`, `kLocation`, `kObject`, `kObjectMono` (a
channel range folded to one mono dynamic object), `kProgramme1`/`kProgramme2` (dual mono).
`trim_db` is a linear-gain trim in `[-24, +24]`, applied wherever that channel's content reaches
the stream — a `route()`-built `Routing` gain entry for `kLocation` rows, or the object plane's
own gain for `kObject`/`kObjectMono` (`route()` contributes nothing for those: object audio
reaches the stream through the Atmos path, not `Routing`).

`route()` returns `nullopt` if `sources` is empty, if two rows target the same location, or if
the target plan cannot express a requested location at all — it does **not** require every
target channel to be filled; an unassigned coded channel is simply silent, which
`Assignment::unassigned(sources)` reports back for a caller (a GUI's warning banner) to show.
`dual_mono_routing()` is the same idea specialised to 1+1's two independent programmes, one
channel on each. `format_assignment`/`parse_assignment` round-trip the whole assignment through
the CLI's `map=` grammar (`kAssignmentSyntax` documents it in full).

---

See also: [Encoding E-AC-3](encoding-eac3.md) — `AccessUnitEncoder`/`AccessUnitConfig`, what a
resolved `ChannelPlan` is built for; [Header map](header-map.md) — `ac3/encoder/plan.hpp`'s
automatic single-source `route()` overload, not covered on this page.
