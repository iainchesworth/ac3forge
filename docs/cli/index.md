# ac3cli

`ac3cli` is the command-line front end over `ac3::forge` — twenty-five commands covering
synthesis, file encoding/decoding, container wrapping, inspection, and live capture/playback.
Every command it can run is backed by the same public library documented under
[Library](../library/index.md); every codec and format decision lives in the library, and the
CLI keeps only small local helpers of its own (the DASH MPD document wrapper `fmp4` writes, the
keyframe-file parser behind `atmos-path`/`atmos-encode`).

Run it with no arguments for the full usage text — the command list in [Commands](commands.md)
is transcribed from it, and re-checked against a built binary at each release.

```bash
ac3cli
```

## Version

```bash
ac3cli --version
```

Prints the semantic version plus git provenance — commit, branch, and a dirty flag. The version
itself is derived from the nearest reachable `v*` git tag at configure time, so it tracks the
latest release tag (see [Releasing](../releasing.md) and `cmake/GitVersionDerivation.cmake`);
the rest is stamped in at build time by `cmake/GenerateVersion.cmake`:

```
ac3forge 0.5.0-beta.1
  release: v0.5.0-beta.1
  commit:  971a547390ff21560e370fb5cb2a22ef362f75de
  branch:  main
  target:  Windows x86_64 (MSVC 1951)
```

`--version` (or its `-v` alias) is a flag, not one of the twenty-five commands — it's handled
before argument parsing and exits immediately.

## Conventions shared across commands

- **Layouts** (`mono | stereo | 1+1 | 51 | 71 | 512 | 514 | 714`) name a channel bed by
  ear-friendly shorthand. AC-3 only reaches `mono | stereo | 1+1 | 51`; anything wider needs
  the dependent substreams that only E-AC-3 has. A layout can also be a comma-separated
  [Table E2.5](../library/channel-plans-and-routing.md) location list
  (e.g. `L,C,R,LFE,Vhl,Vhr`) for a channel set none of the named layouts cover — AC-3 accepts
  one too, as long as it needs no dependent substream — see
  [Options & grammars](metadata-options.md) for the full grammar.
- **`out.ac3` vs. `out.ec3`** is how commands tell AC-3 output from E-AC-3 output; extensions
  aren't enforced, they're just the convention the examples follow.
- **`-` means stdin or stdout** for `encode`, `eac3-encode`, `atmos-encode` and `decode`'s WAV/
  AC-3/E-AC-3 path arguments — the conventional Unix pipe convention, so a WAV or stream never
  needs to touch a disk at all:

  ```bash
  ac3cli encode - - 448 couple < in.wav > out.ac3
  ac3cli decode - - < out.ac3 > out.wav
  ```

  Everything else about the command is unchanged; only the argument's meaning changes from "open
  this path" to "use the standard stream instead". Windows needs no special handling on the
  caller's part — ac3cli puts stdin/stdout into binary mode itself before the first byte crosses
  either one.
- **Metadata options** (`drc=`, `heavy`, `dialnorm=`, `cmixlev=`, …) can follow the positional
  arguments of any encoding command, in any order — see
  [Options & grammars](metadata-options.md), including which commands ignore which options.
- **PCM-carrying commands report per-channel peak/RMS levels when they finish**; `record` meters
  live. With `-` as the output path, every encode and decode path sends that report (and the rest
  of its status text — `dialnorm=auto`'s measurement line and the `src=`/`map=` multi-source
  paths' summary/routing/levels report included) to stderr, so it never lands in the middle of
  the piped stream.
- **Commands needing audio hardware** (`devices`, `record`, `monitor`, `live`, `outputs`, `play`)
  report themselves unavailable on a build with no capture/passthrough backend, rather than
  failing to link — see the per-OS Platform notes pages ([Windows](../platforms/windows.md),
  [Linux](../platforms/linux.md), [Raspberry Pi](../platforms/raspberry-pi.md),
  [macOS](../platforms/macos.md), [Android](../platforms/android.md)) for what's actually
  hardware-confirmed on each OS.

## Next

- [Commands](commands.md) — all 25 commands, grouped and with real usage text.
- [Options & grammars](metadata-options.md) — the `drc=`/`heavy`/`dialnorm=`/… options grammar,
  the `tools` argument grammar, and the full layout/location-list grammar.
- [Concepts](../concepts/index.md) — if `bsid`, `syncframe`, or `JOC` aren't already familiar.
