# macOS (TBD)

!!! warning "No Mac has ever been used with this project"
    There is no macOS host available to this project. The `macos-llvm` CI leg exists in
    [`.github/workflows/ci.yml`](https://github.com/iainchesworth/ac3forge/blob/develop/.github/workflows/ci.yml)
    and configures a `config-macos-llvm` / `config-macos-llvm-debug` preset pair, but it runs
    `continue-on-error` and has never actually executed anywhere — there is nothing to run it
    on.

## What "should" theoretically work

The codec core has no platform dependency. `src/lib/CMakeLists.txt` selects a per-platform
directory for the three features that touch sound hardware — capture, monitor playback and IEC
61937 passthrough — and on macOS that falls back to the same no-backend implementation used on
a Linux machine without `libasound2-dev`: its entry points return `kNoBackend`, so those
features would report themselves unavailable rather than failing to link.

On that basis, the rest of the codec — encode, decode, and the CLI's file-based commands — is
*expected* to work on macOS (Apple Silicon, via the `arm64-macos-llvm` vcpkg triplet and Clang
21). But "expected" is as far as it goes: this has never been observed, built, or tested by
anyone, on CI or otherwise.

## Packaging

No packaging leg exists in practice. DragNDrop is mentioned in the README and
[Building from source](../building.md#packaging) as a CPack possibility on macOS, the same way
NSIS is on Windows and DEB/RPM are on Linux — but it has never been exercised, and there is no
verified `cpack --preset pack-macos-llvm` sequence to document here.

---

If you get this running on a Mac, that's genuinely new information for this project — consider
filing an issue with what you found.
