# Contributing to ac3forge

## Build and test

Setup is in [docs/BUILDING.md](https://github.com/iainchesworth/ac3forge/blob/main/docs/building.md). The short form, from a Developer PowerShell:

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

Everything must pass before you push. There are no known-failing tests and no skips; if
something fails, that is your change or a genuine regression, not noise.

## The clean-room rule

This is the constraint the whole project rests on. Breaking it makes the code unusable.

- Every table and algorithm is transcribed from the published standard — ATSC A/52:2018, or
  for the object layer ETSI TS 103 420 and TS 102 366 — with its section or table number cited
  in a comment.
- Open-source encoders (FFmpeg, Aften, anything else) may be consulted for **architecture
  lessons only**. Never transcribe code. The spec contains every table, so there is never a
  need to.
- One exception, normative by construction: TS 103 420 ships its JOC Huffman tables *as* a C
  file in its companion archive. That file is the standard, not an implementation of it.

If you cannot cite where something came from, it does not go in.

## Code conventions

**C++23, and use it.** `std::expected` for recoverable failure, `std::span` for borrowed
sequences, `std::print`/`std::format` for output, designated initializers for configuration
structs, `constexpr` and `consteval` for anything computable at build time. The window tables
and several spec-table self-checks are `consteval` — a table that is wrong fails the build
rather than a test.

**Warnings are errors.** `ac3::warnings` is linked privately into every first-party target,
including `examples/`. That includes `-Wsign-conversion` and its MSVC equivalents, which in
this codebase means a lot of explicit `static_cast<std::size_t>` on indices. Add the cast; do
not suppress the warning.

**No exceptions for stream-level failure.** A malformed bitstream, an out-of-range
configuration or a missing file are all expected conditions and return `std::expected`. A
programming error — the wrong number of samples in a frame — may assert.

**No allocation on the render path.** `spatial::BedRenderer::render_block` and the capture
ring are called at block rate; allocation there is a bug even when it works.

**`.clang-format` is checked in.** Run it.

## Comments explain why, with a citation

The single most useful thing in this codebase is a comment saying which part of the standard a
line implements and what would go wrong otherwise. Comments that restate the code are noise;
comments that record a decision are the reason the code can be maintained at all.

Good:

```cpp
// A/52 §5.4.4.1 puts aux user data at the END of the auxbits field, immediately
// before auxdatal, "so a decoder can find and unpack the auxdatal user bits
// without knowing the value of nauxbits" - nauxbits being unknowable until the
// whole frame has been decoded. So the container is not appended after the
// padding; the padding is what gets pushed in front of it.
```

That says what the spec requires, quotes the clause, and explains the non-obvious consequence.
A reader who wonders why padding comes first has their answer without opening the PDF.

Not useful:

```cpp
// Write the aux data.
```

Where behaviour is deliberately narrower than the standard, say so and say why — see the
decoder's header for the pattern. "Deliberately unsupported (clean errors, not wrong audio)"
is a design statement; a silent gap is a bug waiting to be found by someone else.

## Validation discipline

Two rules, both learned the hard way. Ignore either and your tests will pass while the code is
broken.

### Test with real audio, from frame 1 onward

**Silence is not a test signal.** With all SNR offsets at zero, §7.2.2.1.1 defines an all-zero
bit allocation: no mantissa data exists and the frame is pure syntax. A silent frame therefore
exercises almost none of the encoder, and passes whatever you have done to the parts it skips.

**Frame 0 is not a test either.** The MDCT overlap buffer starts at zero, so the first frame's
transform is a special case. Frame-layout errors, overlap-state errors and rate-accumulator
errors all show up from frame 2 onward and not before.

So: at least three frames, of material with actual content. Different content per channel when
the test is about channel order or separation — identical tones in two channels cannot
distinguish "the surrounds were overwritten correctly" from "the dependent substream was
ignored".

### Prove the test can fail

A regression test that has never failed is a test you have no evidence about. After writing
one, **reintroduce the bug it is meant to catch and confirm the test fails.** Then revert.

This is not optional ceremony. Several tests in this repo would have passed against the bug
they were written for, and were only fixed because someone checked.

## Oracles

Ranked by how much they prove. Prefer the strongest one available for what you are changing.

1. **The in-repo decoder.** Fully normative and sharing the encoder's core. Strongest for
   anything both sides implement, and the *only* oracle for 7.1.4.
2. **FFmpeg.** External and independent. Always strict-decode:
   `ffmpeg -v error -err_detect crccheck+bitstream+buffer+explode -i out.ac3 -f null -`.
   Without `-err_detect`, FFmpeg conceals errors and a broken stream looks fine. FFmpeg is the
   only oracle for Annex E coupling, spectral extension and AHT.
3. **The Python references in `tools/`.** Independent transcriptions of the same spec text.
   Weaker than a decoder — two transcriptions can share a misreading — but they catch slips a
   self-consistent round trip cannot.
4. **Dolby's Reference Player and Media Encoder**, for object-layer syntax.

Neither decoder covers everything, and the gaps do not overlap: see the [verification-gap
table](https://github.com/iainchesworth/ac3forge/blob/main/README.md#verification-gaps). If your change lands in a cell with no oracle, say so in
the commit message and cover it bit-by-bit instead.

## Documentation

The examples in [docs/library/](https://github.com/iainchesworth/ac3forge/blob/main/docs/library/index.md) are excerpts from programs in
[`examples/`](examples/), which are build targets and `ctest` entries. If you change a public
API, update the example — the build will tell you if you forget. Do not add a snippet to the
docs that is not backed by a compiled file.

If you add a capability or find a new limitation, the tables in [README.md](https://github.com/iainchesworth/ac3forge/blob/main/README.md) are the
authority and must be updated with it. [docs/project/history.md](https://github.com/iainchesworth/ac3forge/blob/main/docs/project/history.md) is a
record of past work and is not maintained against the current state.

## Commits

**Never use a `Co-Authored-By` trailer.** This is absolute, and applies whatever tooling you
are using.

Write the subject as what the change does and, where it fits, why — the existing log is the
style guide:

```
cli: one command table, so an argv index cannot be quietly wrong
integration: drop the duplicate AC-3 channel map
```

Reference the spec section in the body when the change is a spec question. If a commit fixes
something an oracle found, say which oracle.

## Reporting a problem

Include the exact command, the stream if you can attach one, and what the oracle said. For a
decode problem, say which decoder — "it does not play" is not actionable when the in-repo
decoder and FFmpeg refuse different, documented things.
