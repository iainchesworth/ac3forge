# ADM / BW64 reading: `ac3adm::ac3adm`

`ac3adm/ac3adm.hpp`, library `ac3adm::ac3adm`. A standalone BW64/RF64 + Audio Definition Model
(ADM) parser: the professional delivery format Netflix's and Apple's own Atmos ingest pipelines
require. Like `matroska::matroska`, `mp4::mp4` and `mpegts::mpegts`, it links nothing from
`ac3::forge` — it has no idea AC-3, E-AC-3 or the JOC/Atmos object layer exist. It differs from
those three in one way: they are container **writers**, this is a container **reader**, because
that's the direction a professional master needs to travel to reach this project's own encoder
in the first place.

Mapping the graph this module parses onto `ac3::oba::AtmosEncoder` is a separate module,
[`ac3::admbridge`](adm-bridge.md) (done); driving the two together end to end — a real ADM BWF
master straight to a DD+ JOC E-AC-3 stream — is also done: `ac3cli atmos-adm` (see
[Commands](../cli/commands.md)) and
[`examples/encode_adm.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/encode_adm.cpp). This page and
[`examples/read_adm.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/read_adm.cpp) only demonstrate this module's own API — opening a file and walking the
parsed graph; `encode_adm.cpp` is the one that shows the full pipeline.

**Opt-in, unlike every other module in this library.** `AC3FORGE_BUILD_ADM` defaults **off**, and
turning it on additionally needs `-DVCPKG_MANIFEST_FEATURES=adm` (see
[`vcpkg.json`](https://github.com/iainchesworthlabs/ac3forge/blob/main/vcpkg.json)) to resolve its Boost dependency:

```bash
cmake --preset config-windows-msvc-debug -DAC3FORGE_BUILD_ADM=ON -DVCPKG_MANIFEST_FEATURES=adm
```

Every other target in this project — `ac3cli`, `ac3gui`, `ac3tests`, every other example — builds
identically whether `AC3FORGE_BUILD_ADM` is on or off; nothing links `ac3adm::ac3adm`
unconditionally. See "Why opt-in" below for the reasoning.

```cpp
const auto document = ac3adm::parse_bw64(fixture_path);
if (!document) {
    std::printf("parse_bw64 failed: %.*s\n", static_cast<int>(ac3adm::describe(document.error()).size()),
                ac3adm::describe(document.error()).data());
    return 1;
}
```

```cpp
for (const auto& programme : document->model.programmes) {
    std::printf("  programme %s (%s) -> %zu content(s)\n", programme.id.c_str(), programme.name.c_str(),
                programme.content_refs.size());
}
for (const auto& object : document->model.objects) {
    std::printf("  object %s (%s), start=%.5fs, %zu track UID ref(s)\n", object.id.c_str(), object.name.c_str(),
                object.start_s, object.track_uid_refs.size());
}
```

Full program: [`examples/read_adm.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/read_adm.cpp) — writes a small but genuinely valid BW64 fixture (adapted
from Recommendation ITU-R BS.2076-2's own worked "Car" object example) to a temp file, since a
real ADM BWF master is production audio this project has no license to embed, then parses it
back and prints what it found.

## What gets parsed

- **The container** (Recommendation ITU-R BS.2088-1, Annex 1): `<fmt >`/`<data>` integer PCM
  (8/16/24/32-bit — see "Known limitation" below for float32), the `<ds64>` 64-bit size table for
  `RF64`/`BW64`-headed files, `<chna>` (the track-number ↔ ADM-ID join table) and `<axml>` (the
  embedded ADM XML document itself). A plain `RIFF` header is accepted too, for the (very common)
  case of a master that stays under the 4 GB threshold RF64 exists to lift.
- **The ADM object graph** (Recommendation ITU-R BS.2076-2, Annex 1): `audioProgramme` →
  `audioContent` → `audioObject` → `audioPackFormat`/`audioChannelFormat` (with its
  `audioBlockFormat` time-divisions — position, gain, width/height/depth, `channelLock`,
  `jumpPosition`, HOA order/degree/normalization) → `audioStreamFormat`/`audioTrackFormat` →
  `audioTrackUID`. See [`ac3adm/model.hpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/include/ac3adm/model.hpp) for exactly which sub-elements are carried and which
  are deliberately out of phase 1's scope (`zoneExclusion`, `objectDivergence`, `screenRef`, the
  Matrix/Binaural-specific sub-elements, and loudness metadata — `ac3::meta::loudness` already
  measures loudness independently).

**`model` always includes BS.2076-2 Annex A's "common definitions".** libadm's own `parseXml()`
starts every document from a copy already populated with the standard's ~940 predefined
pack/channel/stream/track/block formats (one set per standard loudspeaker layout and first- to
third-order HOA component) and merges the file's own content into it, so that a file referencing
a common-definition ID (e.g. a stereo bed's pack format `AP_00010002`) without locally
re-declaring it still resolves. This module keeps that merge rather than filtering it back out:
phase 2 needs exactly this, a pack/channel/stream/track format reference that resolves regardless
of whether the file re-declared it — so `model.pack_formats`/`channel_formats`/`stream_formats`/
`track_formats` are never just "what this one file defined". `model.programmes`/`contents`/
`objects`/`track_uids` are unaffected (the common set defines none of those four).

`AdmDocument` — the `parse_bw64` result — holds all three pieces together: `model` (the graph
above), `chna` (the join table, one `ChnaEntry` per physical-track-to-ADM-ID row), and `audio`
(the decoded PCM, one `std::vector<float>` per channel, same `[-1, 1)` normalization convention
`ac3::io::WavData` uses). `AdmError` covers open/parse failure — `kCannotOpen`, `kNotRiff`,
`kMissingFmt`, `kMissingData`, `kUnsupportedFormat`, `kMalformedXml`, `kMalformedAdm`, `kOther`;
see [`ac3adm/ac3adm.hpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/include/ac3adm/ac3adm.hpp) for the full list. In practice, the two libraries underneath this
module (see below) report almost everything through one broad exception family each, so most
real failures currently surface as `kCannotOpen` (bad/truncated container), `kMalformedXml` (axml
isn't well-formed XML) or `kMalformedAdm` (well-formed XML that isn't a valid ADM document) — see
[`src/ac3adm/src/adm.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/src/adm.cpp)'s own comments for exactly which library exception maps to which `AdmError`.

## Built on the EBU's own reference implementations

Unlike every other module in this project, `ac3adm::ac3adm` is not a from-scratch, dependency-free
implementation. It is a thin translation layer over two vendored third-party libraries, fetched
via CMake `FetchContent` (see [`src/ac3adm/CMakeLists.txt`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/CMakeLists.txt)):

- **[libbw64](https://github.com/ebu/libbw64)** (Apache-2.0, header-only, no dependency of its
  own) — the BW64/RF64 chunk-walking and PCM-decoding layer.
- **[libadm](https://github.com/ebu/libadm)** (Apache-2.0) — the ADM XML object model: parsing,
  schema validation, and the full element graph.

Both are maintained by the BBC/IRT team that also authored the underlying ITU-R Recommendations
(BS.2088-1, BS.2076-2) themselves — using them means this module's own code only has to translate
an already-validated object graph into `ac3adm`'s own types
([`src/ac3adm/src/adm_model.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/src/adm_model.cpp)), rather than re-implementing container-walking and XML/schema
validation this project has no comparative advantage in getting exactly right on the first try.
An earlier attempt at exactly that hand-rolled approach is what prompted switching to these
libraries instead.

Neither library's own types appear in `ac3adm::ac3adm`'s public headers
(`ac3adm/ac3adm.hpp`, `ac3adm/model.hpp`) — they are translated into this module's own types at
the boundary and stay an implementation detail, the same way this project keeps every other
vendored dependency (e.g. Catch2 in `tests/`) out of its own public API. One practical reason
beyond the usual "don't leak a dependency's API" one: libadm's own public C++ namespace is `adm`,
which is why this module is `ac3adm` and not simply `adm` — the two would otherwise collide
(`adm::AudioObject`, `adm::TypeDefinition`, `adm::Position`, ... are all defined by both).

## Why opt-in

libadm needs several Boost header libraries (Optional, Variant, Range, Iterator, Functional,
Format, Integer and Rational — the exact list confirmed by grepping libadm 0.14.0's own
`#include <boost/...>` directives, not guessed from its README, which undersells it: Rational
and Integer aren't mentioned there at all, but `adm/utilities/time_conversion.hpp` needs both).
Every other dependency in this project is either in-tree or a single small vcpkg
package (Catch2); pulling in Boost is a materially bigger footprint, so it is deliberately
**opt-in rather than default-on** — `AC3FORGE_BUILD_ADM` defaults `OFF` (unlike
`AC3FORGE_BUILD_MATROSKA`/`AC3FORGE_BUILD_MP4`/`AC3FORGE_BUILD_MPEGTS`, which default `ON`), and
turning it on needs the dedicated `adm` vcpkg feature to resolve those Boost packages. A build
with `AC3FORGE_BUILD_ADM=OFF` (the default) never touches `find_package(Boost)`, never fetches
libbw64/libadm, and behaves identically to a build of this project before this module existed.

For the same reason, `ac3adm::ac3adm` is **not** part of the installed `find_package(ac3forge)`
package (see [the library overview](index.md)) and is **not** wired into the Android/Shield NDK
build — see `src/ac3adm/CMakeLists.txt`'s own header comment for both.

## Known limitation: float32 PCM

Only integer PCM (8/16/24/32-bit) is currently supported. libbw64's own `<fmt >` parsing rejects
any other `formatTag` outright — including IEEE-float (`WAVE_FORMAT_IEEE_FLOAT`, formatTag 3) —
at open time (`"format unsupported: <tag>"`), which this module surfaces as `AdmError::
kCannotOpen`. A float32 source file is rejected, not silently misread as integer PCM. Virtually
every real ADM BWF master is 16- or 24-bit integer PCM in practice (EBU Tech 3306/BS.2088-1
Annex 2 §2's own PCM-only framing), so adding float32 support has not been worth doing for
phase 1 — see [`ac3adm/model.hpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/include/ac3adm/model.hpp)'s own `PcmAudio` comment.

---

See also: [File I/O](file-io.md) — the plain-WAV reader this module's container-parsing
deliberately does not share an implementation with, despite the family resemblance;
[ADM → Atmos bridging](adm-bridge.md) — `ac3::admbridge`, which maps this graph onto
`ac3::oba::AtmosEncoder`; [Spatial & Atmos objects](spatial-and-atmos.md) — the
`ac3::oba::AtmosEncoder`/`ac3::oba::motion` surface that bridge drives.
