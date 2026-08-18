# Homebrew formula and cask

`Formula/ac3forge.rb` packages `ac3cli` — the CLI only, built from the release source
tarball. `Casks/ac3gui.rb` packages `ac3gui` — the GUI, as the prebuilt `.app` bundle from a
release's DragNDrop `.dmg`. Both are staged here for local validation against this repo before
being copied into a personal tap, **not** submitted to `homebrew-core`.

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

**The formula:** just `ac3cli` — `AC3FORGE_BUILD_CLI=ON`, GUI/tests/examples/fuzzers off, same
reasoning as the vcpkg port ([`packaging/vcpkg-port/ac3forge/`](../vcpkg-port/ac3forge/))
staying library-only but pointed the other way: Homebrew formulae are for end-user tools, not
`find_package()`-consumed libraries, so this ships the thing vcpkg deliberately does not.

**The cask:** just `ac3gui.app`, the prebuilt bundle from a tagged release's
`ac3forge-*-Darwin.dmg` (`cmake/Packaging.cmake`). A Cask, not a Formula, is the right shape
for a bundled `.app` — Homebrew formulae build from source, and a Qt6 GUI app is idiomatically
distributed prebuilt and signed (or, here, prebuilt and *not* Apple-signed — see the cask's own
`caveats` block). `Casks/ac3gui.rb` now points at a real release: `v0.8.0-beta.2` is the first
tag whose `macos-llvm` CI leg builds `AC3FORGE_BUILD_GUI=ON` (see
[docs/platforms/macos.md](../../docs/platforms/macos.md#gui-on-macos)), so it's the first
`ac3forge-*-Darwin.dmg` that actually contains `ac3gui.app` — `version`/`sha256` are pinned from
that release, not placeholders. Every release after this one still needs the same per-release
bump the sibling Formula gets; see the cask file's own header comment.

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
see [docs/releasing.md](../../docs/releasing.md#homebrew-formula-and-cask) for the per-release update
flow.

The cask now points at a real, downloadable `.dmg` (v0.8.0-beta.2), so it can be validated the
same way, from a macOS machine with Homebrew installed:

```bash
brew audit --cask ./packaging/homebrew/Casks/ac3gui.rb
brew install --cask ./packaging/homebrew/Casks/ac3gui.rb
brew uninstall --cask ac3gui
```

Not yet run for real — this repo's CI has no Homebrew, same as the Formula above, so this is
manual and macOS-only too. Run it before copying the cask into the tap.
