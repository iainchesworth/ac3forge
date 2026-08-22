# Homebrew Cask for ac3gui, ac3forge's Qt6 GUI front end.
#
# A Cask, not a Formula, is the right shape here: ac3gui ships as a prebuilt
# .app bundle inside each release's DragNDrop .dmg (cmake/Packaging.cmake),
# not as something a user builds from source - the same reasoning
# Formula/ac3forge.rb's own header comment and packaging/homebrew/README.md
# already gave for deferring this file. Formula/ac3forge.rb stays the
# CLI-only, build-from-source package (AC3FORGE_BUILD_CLI=ON); this Cask is
# the GUI-only, prebuilt-binary package - two independent installs, matching
# Homebrew's own Formula-vs-Cask split (build-from-source end-user tool vs.
# bundled .app), not a replacement for the Formula.
#
# Staged here (packaging/homebrew/Casks/ac3gui.rb), the same way the Formula
# was, for validation against a real release before being copied into a
# personal tap (homebrew-ac3forge) as Casks/ac3gui.rb - see
# packaging/homebrew/README.md. Copying it into the tap and pushing there is
# a separate, manual step - not done as part of landing this file.
#
# v0.8.0-beta.2 is the first tagged release whose macos-llvm leg builds
# AC3FORGE_BUILD_GUI=ON (see docs/platforms/macos.md#gui-on-macos), so it is
# the first release whose ac3forge-*-Darwin.dmg actually contains
# ac3gui.app - version/sha256 below are real values pinned from that
# release, not placeholders. **Every release tag** after this one still
# needs the same follow-up update "Every release tag" in
# docs/releasing.md#homebrew-formula-and-cask already documents for the
# sibling Formula: bump version, recompute sha256 from that release's own
# ac3forge-*-Darwin.dmg, validate locally, then copy into the tap.
cask "ac3gui" do
  version "0.8.0-beta.2"
  # Pinned from v0.8.0-beta.2's actual release asset (GitHub's own reported
  # digest for ac3forge-0.8.0-Darwin.dmg - the same CPACK_PACKAGE_CHECKSUM
  # SHA512 cmake/Packaging.cmake also computes and publishes alongside it,
  # just a different digest algorithm; Homebrew Casks pin sha256). If
  # `brew install` reports a mismatch, trust brew's reported hash over this
  # one and update it here.
  sha256 "1a18bac9ffecc1e5e57ea665d8ee1b307b950d3df1b35847db54052c82012bec"

  # CPack's dmg filename carries only MAJOR.MINOR.PATCH
  # (cmake/Packaging.cmake's CPACK_PACKAGE_FILE_NAME), dropping any
  # "-beta.N" pre-release suffix the git tag itself carries - the same split
  # Formula/ac3forge.rb's install block works around for the source tarball
  # (there via DERIVED_VERSION_OVERRIDE=v#{version}), just read the other
  # way here since the Cask consumes a prebuilt filename instead of naming
  # its own.
  dmg_version = version.major_minor_patch

  url "https://github.com/iainchesworthlabs/ac3forge/releases/download/v#{version}/ac3forge-#{dmg_version}-Darwin.dmg"
  name "ac3gui"
  desc "Qt6 GUI for ac3forge, a clean-room AC-3/E-AC-3 encoder, decoder and Atmos object-layer toolkit"
  homepage "https://github.com/iainchesworthlabs/ac3forge"

  # arm64-macos-llvm is the only vcpkg triplet macOS CI builds against
  # (cmake/toolchains/macos.llvm.toolchain.cmake) - there is no x86_64 macOS
  # leg or release artifact to fall back to.
  depends_on arch:  :arm64
  # cmake/toolchains/macos.llvm.toolchain.cmake pins CMAKE_OSX_DEPLOYMENT_TARGET
  # to 13.3 (Ventura) for C++23 libc++ feature availability - see that
  # file's own header comment.
  depends_on macos: ">= :ventura"

  app "ac3gui.app"

  caveats <<~EOS
    ac3gui is not Apple-notarized or code-signed. release.yml
    (.github/workflows/release.yml) signs release artifacts with GPG (a detached
    .asc) and attests build provenance via Sigstore/OIDC - neither is Apple code
    signing. macOS Gatekeeper will very likely refuse to open the app on first
    launch ("cannot be opened because the developer cannot be verified") until you
    right-click ac3gui.app in Finder and choose Open, or run:
      xattr -dr com.apple.quarantine "#{appdir}/ac3gui.app"
    This Cask itself is unverified on real hardware, same as every other macOS
    claim in this project - see docs/platforms/macos.md, which documents each one
    as CI-only until someone with a Mac checks it by hand.
  EOS
end
