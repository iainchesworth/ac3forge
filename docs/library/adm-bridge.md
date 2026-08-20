# ADM → Atmos bridging: `ac3::admbridge`

`ac3/admbridge/bridge.hpp`, `ac3/admbridge/coordinates.hpp`, library `ac3::admbridge`. Roadmap item
B1 phase 2 of 3 (see [ROADMAP.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/ROADMAP.md)): maps the ADM object graph
[`ac3adm::ac3adm`](adm.md) (phase 1) parses from a BW64/ADM master onto
[`ac3::oba::AtmosEncoder`](spatial-and-atmos.md)'s input shape — one `ac3::oba::ObjectPath` plus
one mono PCM span per bed speaker feed or dynamic object, ready to drive `encode_frame()` in a
loop. This module is the mapping/bridge library and its tests only; driving it end to end is
phase 3 — `ac3cli atmos-adm` and [`examples/encode_adm.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/encode_adm.cpp), both **done**, see
[Commands](../cli/commands.md).

**Opt-in, gated by the same flag as `ac3adm::ac3adm`.** `ac3::admbridge` depends on both
`ac3adm::ac3adm` and `ac3::forge`, so it is meaningless without `AC3FORGE_BUILD_ADM=ON` and is
built as part of the same `add_subdirectory` block — no separate `AC3FORGE_BUILD_ADMBRIDGE` option
exists. See [ADM / BW64 reading](adm.md) for the exact CMake invocation.

```cpp
const auto document = ac3adm::parse_bw64(path);
if (!document) { /* ... */ }

const auto bridged = ac3::admbridge::build(*document);
if (!bridged) {
    std::printf("build failed: %.*s\n",
                static_cast<int>(ac3::admbridge::describe(bridged.error()).size()),
                ac3::admbridge::describe(bridged.error()).data());
    return 1;
}

ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, static_cast<int>(bridged->channel_count())};
// Per frame: slice bridged->pcm[i] to the frame's sample range, evaluate
// ac3::oba::evaluate_placements(bridged->paths, t), call encoder.encode_frame(...).
```

## Why a new, standalone module

Two hard constraints rule out folding this into either side it bridges:

- `ac3adm::ac3adm` is documentedly codec-blind — its own header comments and this project's design
  keep it with zero dependency on `ac3::forge`/`ac3::oba`, and that does not change here.
- `src/lib` (`ac3::forge`, `ac3::oba::AtmosEncoder`) is always built, unconditionally, by every
  configuration of this project. It cannot gain a dependency on the opt-in, Boost-requiring
  `ac3adm` without breaking every default build.

`ac3::admbridge` is therefore its own module (`src/admbridge/`), PUBLIC-linking both — the same
shape `ac3::signing` uses for its own `ac3::forge` dependency. It is not part of the installed
`find_package(ac3forge)` package, for the same reason `ac3adm::ac3adm` itself is not (see
[ADM / BW64 reading](adm.md)'s own "Why opt-in" section): consume it via `add_subdirectory`
in-tree. ROADMAP.md's B2 entry (a future DAMF `.atmos`/`.atmos.metadata`/`.atmos.audio` reader)
names this module as the "mapping layer" it plans to share once it exists — another reason to keep
this logic independent of `ac3adm`'s own BW64/ADM-XML-specific parsing, even though
`ac3adm::AdmDocument` is the only input shape today.

## What gets mapped

- **Bed vs. dynamic object** is decided by the `TypeDefinition` of the `audioObject`'s own resolved
  `audioPackFormat`(s) — `AudioObject` itself carries no type of its own. `kDirectSpeakers` becomes
  a bed speaker feed; `kObjects` becomes a dynamic object. `kMatrix`, `kHoa`, `kBinaural`,
  `kUserCustom`, `kUnknown`, a nested `audioPackFormat`, or an object whose several packs disagree
  with each other all fail clearly with `BridgeError::kUnsupportedType` rather than being silently
  mishandled — none of them map onto `AtmosEncoder`'s plain position+gain+lfe_send object model
  without a design of their own this phase does not attempt.
- **`AtmosEncoder` has no separate bed-feeding method** — its constructor takes a plain object
  count and `encode_frame()` takes one flat span of objects plus one flat span of placements,
  nothing in that signature distinguishing a bed channel from a dynamic object — so a bed channel
  is represented as an object with an unmoving, pinned placement, the only way the API allows it,
  the same convention every existing caller (`ac3cli`'s `run_atmos_encode`, the GUI's
  `encodeObjects`) already uses. A bed channel whose `speakerLabel` identifies it as the LFE
  (BS.2076-2 Table 12: `LFE`, `LFE1`, `LFE2`) is routed at gain 0 / `lfe_send` 1 instead of panned
  — objects never reach the LFE by panning.
- **Position/gain automation** — `build_channel_path()` walks a channel's `audioBlockFormat`
  sequence into one `ac3::oba::ObjectPath`, implementing BS.2076-2 §10.3's `jumpPosition`/
  `interpolationLength` state machine: `jumpPosition = 0` interpolates continuously across a
  block's *entire* duration; `jumpPosition = 1` jumps (or ramps over `interpolationLength`, when
  given) at the block's *start* and then holds for the rest of it; the first block in a sequence
  always holds across its own span regardless of its own `jumpPosition`. This was checked directly
  against the standard's own text and worked Figs. 7–10 — an earlier reading of just this
  behaviour's *name* ("hold vs. glide") had the two cases backwards relative to what §10.3 actually
  says, corrected once the real clause text was read rather than assumed.
- **Coordinate conversion** (`coordinates.hpp`) — BS.2076-2 §8's polar (azimuth/elevation/distance,
  positive azimuth to the left, positive elevation up) and Cartesian (X right-positive, Y
  front-positive, Z top-positive, `[-1, 1]` unit cube) conventions, both converted to
  `ac3::oba::Position`'s room-anchored `[0, 1]`/`[0, 1]`/`[-1, 1]` one. Checked against the
  standard's own axis-direction text at the cardinal points, and empirically against this
  project's own existing ring-position constants (`tests/test_atmos_motion.cpp`'s `kL`/`kR`/`kSR`)
  — the BS.2076-2 `M+030`/`M-030`/`M-110` speaker-label azimuths reproduce those exact values
  through this conversion.
- **Absolute timeline time** for a channel's automation is `object.start_s + block.rtime_s` — two
  levels, not three. BS.2076-2 Table 24 defines `audioObject`'s own `start` as relative to the
  *programme's* start directly, and §5.6.7 confirms this holds through nesting (a nested object's
  own `start` is never added to its parent's). `audioProgramme`'s own optional `start`/`end`
  (Table 37) is a separate video-alignment concept, not a third term in this sum — and
  `ac3adm::AudioProgramme` does not even carry a `start_s` field to add.
- **Channel resolution** walks `audioProgramme` → `audioContent` → `audioObject` (recursing through
  nested `audioObjects`, with a cycle guard per §5.6.7's own prohibition on reference loops) →
  `audioPackFormat` → `audioChannelFormat`, then resolves each channel's `audioTrackUIDRef`
  through `<chna>` to its physical PCM channel in `AdmDocument::audio`.

## What does not get mapped

`width`/`height`/`depth`/`diffuse`/`objectDivergence` (parsed by `ac3adm`, and per §10.3 also
nominally interpolatable) have no equivalent in `ac3::oba::Keyframe`/`ObjectPlacement` at all —
`AtmosEncoder`'s object model is a pure point source, so every channel this bridge produces is one
too, regardless of what the source ADM data's spread parameters said. `channelLock`/`zoneExclusion`
are parsed but likewise dropped — there is no "nearest bed speaker" or "masked zone" concept
downstream of a fixed 5.1 VBAP ring to map them onto. Both are silent drops, documented here rather
than left for a caller to discover by reading source.

## API

```cpp
enum class BridgeError : std::uint8_t {
    kNoProgramme, kProgrammeNotFound, kUnresolvedReference, kObjectReferenceCycle,
    kUnsupportedType, kChannelTrackMismatch, kNoAudioForTrack, kEmptyBlockSequence,
    kTooManyChannels,
};
std::string_view describe(BridgeError error);

std::expected<ac3::oba::ObjectPath, BridgeError> build_channel_path(
    const ac3adm::AudioChannelFormat& channel, double object_start_s, bool force_lfe);

struct BridgeResult {
    std::vector<std::string> channel_ids;
    std::vector<bool> is_bed;
    std::vector<bool> is_lfe;
    std::vector<ac3::oba::ObjectPath> paths;   // pass directly to evaluate_placements
    std::vector<std::span<const float>> pcm;   // borrowed from the AdmDocument passed to build()
    std::uint32_t sample_rate = 0;
};
std::expected<BridgeResult, BridgeError> build(const ac3adm::AdmDocument& document,
                                               std::string_view programme_id = {});

ac3adm::CartesianPosition polar_to_adm_cartesian(const ac3adm::PolarPosition& polar);
ac3::oba::Position adm_cartesian_to_room(const ac3adm::CartesianPosition& cartesian);
ac3::oba::Position adm_position_to_room(const ac3adm::Position& position);
```

`BridgeResult` is deliberately struct-of-arrays, not one struct per channel — `paths` is directly
usable as the `std::span<const ac3::oba::ObjectPath>` `ac3::oba::evaluate_placements` wants, with
no projection step. `channel_count() <= 15`: `AtmosEncoder`'s own constructor `objects` parameter
is dynamic objects only, with the bed's own LFE bookkeeping as an implicit, always-present 16th
(TS 103 420 §8.3.2.2 caps the total at 16) — the same cap `ac3cli`'s `run_atmos_encode`/
`run_atmos_path` already enforce, reused here rather than re-derived. `sample_rate` is the raw
`ac3adm::PcmAudio::sample_rate`, unconverted — mapping it to `ac3::SampleRate` (and rejecting an
unsupported rate) is left to the caller, the same way every existing WAV-reading entry point
already does that itself.

`bitrate_kbps`/`dialnorm` and every other `AtmosConfig` field are encoding choices ADM data does
not carry at all — `build()` deliberately does not invent defaults for them; constructing
`AtmosConfig` is the caller's job.

## Tests

`tests/test_adm_bridge.cpp` covers coordinate conversion (against BS.2076-2 §8's cardinal points
and this project's own existing ring constants), `build_channel_path`'s full §10.3 state machine
(single block, continuous-glide blocks, instant-jump blocks, ramp-then-hold blocks, the
first-block-always-holds override, the LFE override), `build()`'s graph-walking error paths
(unresolved references, a reference cycle, an unsupported pack type, a channel/track-count
mismatch, the 15-channel cap, default programme selection), and one flagship test that builds a
real byte-level BW64 fixture (two DirectSpeakers bed channels plus one Objects channel that holds
at one ring position and then jumps to another), parses it with the real `ac3adm::parse_bw64()`,
bridges it, and drives a real `ac3::oba::AtmosEncoder`/`ac3::Eac3Decoder` round trip — confirming
the decoded bitstream's channel energy actually lands where the authored ADM positions and hold/
jump timing say it should, the same standard `tests/test_atmos_motion.cpp`'s own flagship test
holds itself to.

`tests/test_cli_atmos_adm.cpp` (phase 3) covers the same fixture shape one level up: it runs the
real, built `ac3cli` binary's `atmos-adm` command as a subprocess against a real ADM BWF file on
disk, then decodes what that binary actually wrote and checks the same channel-energy assertions —
proving the CLI's own argument parsing and its `parse_bw64` → `build` → `AtmosEncoder` wiring, not
just the library API in isolation — plus two error-path cases (`BridgeError::kNoProgramme`, a
malformed/non-RIFF file) confirming `describe()` reaches the terminal rather than an opaque crash
or exit code.

---

See also: [ADM / BW64 reading](adm.md) — the phase-1 parser this module consumes; [Spatial &
Atmos objects](spatial-and-atmos.md) — `ac3::oba::AtmosEncoder`, `ac3::oba::motion`, and
`ac3::oba::Position`'s own room-anchored coordinate convention this module's `coordinates.hpp`
converts into.
