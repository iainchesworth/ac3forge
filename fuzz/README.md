# Fuzzing

libFuzzer harnesses over every place ac3forge parses externally-supplied
binary data. This is the codec's natural attack surface: its whole job is
decoding bitstreams whose structure it cannot control, and the project has
already had one real bug in this class - commit `8386c8f` fixed a decoder
that shifted by an unvalidated exponent, walked outside the range the
reconstruction code assumed by a malformed differential chain, and hit
undefined behaviour on hostile input. It was found by a one-off manual
adversarial audit; this directory makes that kind of input-shape exploration
continuous and automatic instead.

## Why Clang only

libFuzzer (`-fsanitize=fuzzer`) is an LLVM built-in. GCC and MSVC do not ship
it, so everything here requires upstream Clang - specifically the
`linux-llvm` / `macos-llvm` toolchain this project already has presets for
(`windows-llvm` is clang-cl, whose libFuzzer support on Windows this project
has never exercised, so it is deliberately out of scope; see
`fuzz/CMakeLists.txt`'s `CMAKE_CXX_COMPILER_FRONTEND_VARIANT` guard).

`.github/toolchain/03-llvm-toolchain.sh` installs the Clang compiler itself
but not `libclang-rt-<ver>-dev` (the ASan/UBSan/libFuzzer runtime archives):
none of `ci.yml`'s other Clang legs link a sanitizer, so none of them have
ever needed it. `fuzz.yml` installs it as an explicit extra step; a local
Debian/Ubuntu run needs `apt-get install libclang-rt-21-dev` (or your
distro's equivalent) before `fuzz/run.sh` will link.

## Why `-Werror` is off for this build

`cmake/CompilerWarnings.cmake` turns on `-Werror`, and only the Windows MSVC
leg has ever had to satisfy it - the other four (windows-llvm, linux-gcc,
linux-llvm, macos-llvm) are marked `experimental` in `ci.yml` with a real
warning-count debt (sign-conversion, double-promotion, and the like) that is
explicitly scoped to the cross-platform porting task, not this one.
`AC3FORGE_BUILD_FUZZERS` skips linking `ac3::warnings` into `ac3forge`
(`src/lib/CMakeLists.txt`) so a fuzz build can proceed under Clang today
without taking on that unrelated cleanup. This does not relax anything a
fuzzer would catch: ASan and UBSan still fire on real memory and undefined-
behaviour bugs regardless of `-Wsign-conversion`.

## Status at the commit that added this

Like `ci.yml`'s own leg-status table, this is a point-in-time result, not a
standing guarantee - re-run it yourself rather than trusting an old number.

Two full bounded passes ran locally before this landed (Docker: `ubuntu:26.04`
+ LLVM 21, matching CI's `linux-llvm` leg, since this was developed on a
Windows host with no native libFuzzer). The first pass used a Debug build and
was clean but showed pathologically low throughput on the decode harnesses
(2-3 exec/s); switching to `RelWithDebInfo` - libFuzzer's own advice, build
with optimizations on even under sanitizers - fixed that. Numbers below are
the second pass, 180s/harness:

| Harness             | Executions | Corpus grown to  | Result |
|----------------------|-----------:|------------------|--------|
| `fuzz_scan`          |      ~25.9M | 116 files / 1.0MB | clean  |
| `fuzz_ac3_decode`    |       3,011 | 184 files / 5.8MB | clean  |
| `fuzz_eac3_decode`   |       2,796 | 166 files / 7.6MB | clean  |
| `fuzz_wav_read`      |      13,370 | 56 files / 14MB   | clean  |

No crash, hang, or sanitizer report across ~45M total executions between the
two passes; `fuzz/regressions/` is empty as of this commit. `fuzz_scan`'s exec
count dwarfs the decode harnesses' because a format-sniff is orders of
magnitude cheaper than a real IMDCT-and-bit-allocation decode - expected, not
a sign anything is under-tested relative to its own cost.

## Entry points covered

| Harness              | Calls                                                              |
|-----------------------|--------------------------------------------------------------------|
| `fuzz_scan`            | `ac3::io::scan` - format-sniffing before any decoder commits to a layout |
| `fuzz_ac3_decode`      | `ac3::split_frames` + `ac3::FrameDecoder::decode_frame`, one decoder across all frames, the way `ac3cli decode` drives it |
| `fuzz_eac3_decode`     | `ac3::split_access_units` + `ac3::Eac3Decoder::decode_access_unit` (which calls `decode_substream` internally), the way `ac3cli decode` drives it for E-AC-3 |
| `fuzz_wav_read`        | `ac3::io::read_wav` - a realistic input too (a truncated or hand-edited WAV), not only an adversarial one |

`matroska::` was checked and has no read/demux path - `matroska::mux()` only
ever produces bytes from frames already in hand, so there is nothing to fuzz
there. `ac3::io::read_wav` takes a path rather than a byte span, so
`fuzz_wav_read` round-trips libFuzzer's buffer through a scratch file
(`/dev/shm` when available) before calling it - the one unavoidable step
beyond calling the real function directly, since there is no in-memory
overload to call instead.

## Running locally

```bash
# One-time: any Clang 18+ with libFuzzer works; CI pins LLVM 21 the same way
# ci.yml's linux-llvm leg does (.github/toolchain/03-llvm-toolchain.sh).
fuzz/run.sh                    # build, then run every harness for 60s each
fuzz/run.sh fuzz_scan          # just one harness
AC3FORGE_FUZZ_SECONDS=600 fuzz/run.sh   # a deeper local run
fuzz/run.sh regress            # replay seeds + regressions, no mutation (fast)
fuzz/run.sh minimize fuzz_scan fuzz/artifacts/fuzz_scan-crash-<hash>
```

On Windows, run this from WSL or inside a Linux container - there is no
libFuzzer under MSVC or clang-cl here. The commands used to develop this
directory ran inside `docker run ubuntu:26.04` with the repo bind-mounted,
which is exactly what `.github/workflows/fuzz.yml`'s containers do.

See `fuzz/run.sh --help`-equivalent (its own header comment) for the full
environment-variable list.

## Seed corpus

`fuzz/seeds/<harness>/` is a curated, committed bootstrap corpus generated
from ac3forge's own valid output - `fuzz/generate-seeds.sh` drives `ac3cli`
across the layout/codec/Annex-E-tool matrix this project already supports
(every layout token, every tool combination, both codecs, silence and real
audio, coupled and uncoupled, Atmos objects and the bed51 fallback) and
collects the resulting streams. Starting mutation from real, self-consistent
streams is what lets a fuzzer's mutations explore "almost valid" input
instead of spending its budget on bytes that get rejected before line one of
the parser.

Regenerate it with:

```bash
AC3CLI_BIN=build/config-windows-msvc-debug/bin/ac3cli.exe fuzz/generate-seeds.sh
```

(Any *working* `ac3cli` build does - this only needs it to produce valid
streams, not to run instrumented, so the MSVC leg - the one leg proven clean
under `-Werror` - is the practical choice today.)

`fuzz/seeds/` is intentionally small (a few MB) and committed to the repo.
`fuzz/corpus/` - what a real mutation run *grows* into over its time budget -
is not: it is regenerable from the seeds plus a mutation budget, and libFuzzer
corpora can reach hundreds of MB, which does not belong in git history. It is
gitignored; `fuzz/run.sh` creates it on demand.

## When a fuzzer finds something

1. libFuzzer minimizes automatically (or run `fuzz/run.sh minimize <target>
   <path>` on a saved artifact).
2. The minimized input is added to `fuzz/regressions/<harness>/` and
   committed - `fuzz/run.sh regress` (and `fuzz-regress` in CI) replays every
   file there on every run, so a fixed bug can never silently regress.
3. The underlying bug gets a real, spec-grounded fix in the library - never
   just enough to make the fuzzer stop finding it.
4. Before considering it fixed: check out the pre-fix commit and confirm the
   *original* minimized input actually reproduces the crash there. A
   regression test that was never shown to fail is not proof of anything.

## CI

`.github/workflows/fuzz.yml`:

- `fuzz-regress` - replays `fuzz/seeds/` + `fuzz/regressions/` with no
  mutation, on every push/PR to `main`/`develop`. Seconds, not minutes, and
  not marked experimental: a failure here means a previously-fixed bug came
  back, which should always be loud.
- `fuzz-short` - a 60-second-per-harness mutation budget, push only (not
  pull_request, to keep PR turnaround unaffected).
- `fuzz-nightly` - a 10-minute-per-harness mutation budget on a daily
  schedule, plus `workflow_dispatch` with a configurable budget for an
  on-demand deeper run.

`fuzz-short` and `fuzz-nightly` run with `continue-on-error: true`, the same
convention `ci.yml` uses for its other unproven Clang legs: this is the first
time the project has asked Clang to build `ac3forge` with `-Werror` off and
ASan+UBSan+libFuzzer on, and neither job has multiple clean runs behind it
yet. `fuzz-regress` is cheap enough to make a required branch-protection
check once it has proven itself - that is a repository setting this file
cannot declare on its own.

This is a bounded, time-boxed run, not continuous (OSS-Fuzz-style) fuzzing
infrastructure. That is a deliberate scope decision, not a limitation
somebody forgot to lift.
