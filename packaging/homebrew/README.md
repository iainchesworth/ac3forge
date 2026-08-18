# Homebrew formula

`Formula/ac3forge.rb` packages `ac3cli` — the CLI only, built from the release source
tarball. It is staged here for local validation against this repo before being copied into a
personal tap, **not** submitted to `homebrew-core`.

## Why a personal tap, not `homebrew-core`

`homebrew-core` has its own bar a submission has to clear on top of the formula being
technically correct: notability (real, sustained usage — GitHub stars/forks/watchers, not
just "it exists"), a track record of maintenance, and a formula that needs no unusual
patching to build cleanly with Homebrew's own toolchain on every supported macOS version.
ac3forge does not clear that bar yet. A personal tap (`iainchesworthlabs/ac3forge`, i.e. a
`homebrew-ac3forge` repo) has none of those requirements — anyone can `brew tap` it and
`brew install` from it immediately — and is the right home for the formula until a
`homebrew-core` submission is worth making on its own merits. See [Homebrew's Acceptable
Formulae criteria](https://docs.brew.sh/Acceptable-Formulae) for the full bar; that PR, if
and when it happens, is a separate decision from staging the formula here.

## What gets packaged

Just `ac3cli` — `AC3FORGE_BUILD_CLI=ON`, GUI/tests/examples/fuzzers off, same reasoning as
the vcpkg port ([`packaging/vcpkg-port/ac3forge/`](../vcpkg-port/ac3forge/)) staying
library-only but pointed the other way: Homebrew formulae are for end-user tools, not
`find_package()`-consumed libraries, so this ships the thing vcpkg deliberately does not.
The Qt6 GUI (`ac3gui`) is not packaged here — a Homebrew Cask (for a bundled `.app`), not a
Formula, is the right shape for it, and needs its own follow-up; see
[docs/releasing.md](../../docs/releasing.md#homebrew-formula).

## Validating locally

From a macOS machine with Homebrew installed:

```bash
brew install --build-from-source ./packaging/homebrew/Formula/ac3forge.rb
brew test ac3forge
brew audit --formula ./packaging/homebrew/Formula/ac3forge.rb
brew uninstall ac3forge
```

`brew audit` catches most style/metadata issues before they'd surface in a tap or a
`homebrew-core` PR review. There is no Windows or Linux port of Homebrew's formula-build
tooling in this repo's CI, so this validation is manual, macOS-only, and not automated —
see [docs/releasing.md](../../docs/releasing.md#homebrew-formula) for the per-release update
flow.
