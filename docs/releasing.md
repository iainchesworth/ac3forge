# Releasing ac3forge

How to cut a release: what triggers `.github/workflows/release.yml`, what it produces, and how
to set up the optional GPG signing key. Modelled on an earlier project's release process, with
the parts that don't apply to ac3forge (APT/DNF repository publishing, a Docker image, a Home
Assistant add-on) removed.

## Versioning

ac3forge derives its version from git tags, the same way aqualink-automate does.
`cmake/GitVersionDerivation.cmake` runs `git describe --tags --match "v*"` **before**
`project()` in the top-level `CMakeLists.txt` and feeds the result straight into
`project(ac3forge VERSION ...)` - the tag is the single source of truth. Nothing in the tree
hardcodes a version to bump by hand: not `CMakeLists.txt`, and not the root `vcpkg.json`, which
carries no `"version"` field at all - per vcpkg's own schema that field is only required for a
manifest describing a *library* (a port), and this one just declares this project's own
build-time dependencies (Catch2, optionally Boost/Tracy). The staged port's `version-semver` is
what actually tracks releases - see [vcpkg port](#vcpkg-port) below.

So the order is just:

1. Merge to `main`.
2. Tag.
3. Sync `main` back into `develop`: `gh pr create --base develop --head main`, then merge it the
   same way as any other PR. This isn't optional cleanup - `git describe --tags` walks ancestry
   from HEAD, so a tag that exists only on `main`'s side of history is invisible to a `develop`
   build. Skipping this step after the v0.6.0-beta.1 promotion (#180) left `develop` builds
   reporting a stale `v0.5.0-beta.1-...` version - visible in `ac3gui`'s About dialog - until the
   gap was closed with #192.

No version-bump commit, no file to keep in sync - tagging *is* the release decision.

Tags are strict SemVer 2.0.0: `vMAJOR.MINOR.PATCH[-(alpha|beta|rc).N]`, e.g. `v0.2.0` or
`v0.2.0-beta.1`. A tag with a prerelease suffix (or the dispatch form's `prerelease` checkbox)
marks the GitHub Release as a prerelease. The suffix also flows into the build: CMake's
`project()` `VERSION` field can only hold the bare `X.Y.Z` (that's what `PROJECT_VERSION` and
CPack's package version use), but the full tag - suffix included - is carried separately as
`PROJECT_VERSION_FULL` and shows up as `ac3cli --version`'s `version_full` field.

A checkout that can't see any `v*` tag (no history, or a shallow CI clone - see `_build.yml`'s
`fetch_depth` input) falls back to version `0.0.0-dev` rather than failing the build. Ordinary
CI legs stay shallow and always show that fallback; only `release.yml`'s tag-triggered or
dispatched build fetches full history (or gets the version stamped directly via
`-DDERIVED_VERSION_OVERRIDE=`) and shows the real one.

## Pre-release checklist

1. CI green on `main` for the commit you're about to tag.
2. Releases must be **cut from main** - `resolve-version` checks this with
   `git merge-base --is-ancestor` and fails otherwise (dry runs are exempt).
3. Decide the tag.

## Option A: tag-based release (the normal path)

```bash
git checkout main
git pull origin main
git tag v0.2.0
git push origin v0.2.0
```

Prerelease: `git tag v0.2.0-beta.1 && git push origin v0.2.0-beta.1`. Watch the run under
Actions > Release.

## Option B: manual dispatch

Actions > Release > Run workflow, fill in `version` (e.g. `v0.2.0`), `prerelease`, `dry_run`.
The tag does not exist yet when the run starts; `resolve-version` fails fast if it already does.
The `github-release` job pushes the tag itself, at the very end, only after
build/package/sign/attest have all succeeded - so a failed dispatch run leaves nothing behind to
clean up by hand for a real release. For a **prerelease** dispatch specifically, if the tag gets
pushed but a later step still fails, `cleanup-failed-prerelease` deletes the orphaned tag
automatically; a non-prerelease tag is left alone even on failure; deleting a version someone
explicitly declared is a bigger surprise than a maintainer cleaning it up by hand.

## Dry run

Builds and packages every platform without tagging, signing, or publishing anything. Exempt
from the cut-from-main guard, so it can
run from any branch - use it to validate a packaging change before merging.

## Post-release

`gh release create --generate-notes` drafts release notes from merged PRs/commits since the
previous tag - a first draft only. Curating it to the established pattern is a required step,
not optional polish:

1. Update [CHANGELOG.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/CHANGELOG.md) first, if it isn't already current - a `## [x.y.z] -
   YYYY-MM-DD` section (moved down from `## [Unreleased]` if the changes were already logged
   there), [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format. This is the
   authoritative, human-curated record; the GitHub Release body mirrors it, not the other way
   round.
2. Write the GitHub Release body from that CHANGELOG.md section, in this order:
   1. `## What's Changed` (first release) or `## What's Changed since v<prev>`.
   2. A one-line summary of the release.
   3. The changes as `###` subsections - grouped by subsystem, or as *Added*/*Changed*/*Fixed* -
      with **bold lead-in** bullets in user-facing terms, mirrored from the CHANGELOG.md
      section. A short release may use a flat bold-lead-in bullet list instead of subsections.
   4. An `## Artifacts` section: the packages listed under [What gets
      published](#what-gets-published) below, with their SHA-512 checksums, plus a pointer to
      `ac3cli --help`/[docs/quickstart.md](quickstart.md).
   5. `**Full Changelog**: …/compare/v<prev>...v<this>` (keep the one `--generate-notes`
      produced; omit for the first release - there is no previous tag).
   6. For a prerelease, a trailing `> **Pre-release.**` caveat blockquote noting the biggest
      open gap (see [Known gaps](https://github.com/iainchesworthlabs/ac3forge/blob/main/CHANGELOG.md#known-gaps) in the matching CHANGELOG.md
      section).
3. Apply it:

   ```bash
   gh release edit vX.Y.Z[-beta.N] --notes-file notes.md
   ```

4. Verify the release page has all expected artifacts, and that the curated notes render and
   read well.

## vcpkg port

A vcpkg port for `ac3forge` is staged in-tree at
[`packaging/vcpkg-port/ac3forge/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/packaging/vcpkg-port/ac3forge)
(`vcpkg.json`,
`portfile.cmake`, `usage`) and is pending submission to the curated `microsoft/vcpkg` registry -
see [docs/library/index.md](library/index.md) for how a consumer uses it either
way. It installs the library only (`ac3::forge`, plus `matroska::matroska`/`mp4::mp4`/
`mpegts::mpegts` behind their own default-on `matroska`/`mp4`/`mpegts` features - see
`cmake/InstallLibrary.cmake`'s `AC3FORGE_BUILD_MATROSKA`/`AC3FORGE_BUILD_MP4`/
`AC3FORGE_BUILD_MPEGTS`/`AC3FORGE_INSTALL_BOTH_LINKAGES` options), never the
CLI/GUI/tests/examples/fuzzers. `ac3adm::ac3adm` (the ADM/BW64 reader) has no vcpkg feature and
never will while it stays outside `find_package(ac3forge)` entirely - see
[docs/library/index.md](library/index.md).

Any future optional library component follows the same three-step recipe this repo's own
`AC3FORGE_BUILD_<NAME>` options already establish: add the CMake option and its
`cmake/InstallLibrary.cmake` guard first (that part isn't vcpkg-specific), then add a same-named
feature to `packaging/vcpkg-port/ac3forge/vcpkg.json` and one line to `portfile.cmake`'s
`vcpkg_check_features()` call.

**Every release tag, once the port has been merged upstream**, needs a follow-up PR to
`microsoft/vcpkg` - the curated registry has no mechanism to track a moving `main`, so a new
`ac3forge` release is invisible to `vcpkg install` until this happens:

1. Bump `packaging/vcpkg-port/ac3forge/vcpkg.json`'s `version-semver` to the new tag, and
   `portfile.cmake`'s `vcpkg_from_github()` `REF`/`SHA512` to match (`sha512sum` the tag's
   release tarball, or let a first `vcpkg install` attempt report the correct hash).
2. Validate locally first (see below) before touching the upstream fork - a portfile change
   that fails vcpkg's own CI is slower to iterate on there than here.
3. Copy the updated port files into the `microsoft/vcpkg` fork's `ports/ac3forge/`, run
   `vcpkg x-add-version ac3forge` to regenerate `versions/baseline.json`/
   `versions/a-/ac3forge.json` (don't hand-edit these), and open the version-bump PR.

**Validating the port locally**, any time `packaging/vcpkg-port/ac3forge/` or the CMake options
it drives change (whether or not a release is involved):

```bash
vcpkg install ac3forge --classic --overlay-ports=packaging/vcpkg-port --triplet x64-windows
vcpkg install ac3forge --classic --overlay-ports=packaging/vcpkg-port --triplet x64-windows-static
vcpkg install ac3forge[core] --classic --overlay-ports=packaging/vcpkg-port --triplet x64-windows
```

`--classic` is required from inside this repo - the root `vcpkg.json` (manifest mode, for this
project's *own* build-time dependencies) would otherwise shadow the package-name argument.
Check for a clean post-build lint (no "not used"/"missing usage" warnings) and that
`ac3forge[core]` installs without `matroska::matroska`/`mp4::mp4`/`mpegts::mpegts` at all, not
just unlinked.

Fetching a real tag only exercises whatever `AC3FORGE_BUILD_*` options actually existed in that
tagged source - `vcpkg_from_github()`'s `REF` always points at an already-released tag, so a
CMake option added since the last tag (as happened here: `AC3FORGE_BUILD_MP4`/
`AC3FORGE_BUILD_MPEGTS` landed in `develop` after `v0.5.0-beta.1`) can't be exercised through a
real fetch until the *next* tag contains it. To validate a port change against unreleased CMake
options, temporarily swap the `vcpkg_from_github()` block in a scratch copy of `portfile.cmake`
for `set(SOURCE_PATH "<absolute path to this checkout>")`, run the same three commands against
that scratch copy, and discard it once validated - never commit that substitution.

## Publishing to PyPI

Roadmap **F2**: Python bindings (`python/`, see
[docs/library/python-api.md](library/python-api.md)) as the `ac3forge` PyPI package, with wheels
for Windows, macOS and Linux built by `.github/workflows/wheels.yml` via `cibuildwheel`. That
workflow's `build` job runs continuously (every push/PR touching `python/**`, same "buildable is
checked continuously" reasoning as `windows-msvc`'s packaging smoke test above) and always
uploads the wheels it builds as a workflow artifact — that part needs no provisioning and already
works today.

**Publishing them to PyPI is off until a maintainer provisions it**, the same shape as GPG signing
and the Android release keystore below: `wheels.yml`'s `publish` job is gated on both a `v*` tag
push and the `pypi` GitHub environment existing, and uses
[PyPI trusted publishing](https://docs.pypi.org/trusted-publishers/) (OIDC) rather than a stored
API token — there is no `PYPI_API_TOKEN` secret to leak in the first place. **Nobody should ever
generate a long-lived PyPI API token and paste it into a chat with an agent or into a GitHub
secret** — trusted publishing exists specifically so that never has to happen.

One-time setup, done by a maintainer directly on pypi.org and on GitHub:

1. On PyPI, either publish the very first `ac3forge` release by hand (`python -m build python/`
   then `twine upload`, using a temporary scoped token deleted immediately after) to create the
   project, or use PyPI's **pending publisher** mechanism (Your projects → Publishing →
   "Add a pending publisher") to pre-register the trusted publisher for a project name that does
   not exist yet — the second path needs no manual upload at all and is the one to prefer.
2. Either way, register the trusted publisher against this repository: owner
   `iainchesworthlabs`, repository `ac3forge`, workflow `wheels.yml`, environment `pypi`.
3. In the GitHub repo, create an environment named `pypi` (Settings → Environments) — no secrets
   need adding to it; its existence and name are what PyPI's trusted-publisher registration keys
   against, and `wheels.yml`'s `publish` job declares `environment: pypi` so the job has somewhere
   to request the OIDC token from. Optionally add required reviewers on the environment for a
   manual approval gate before a publish actually runs.

Once both sides are set up, pushing a `v*` tag (the same tag that triggers `release.yml`, see
[Option A](#option-a-tag-based-release-the-normal-path) above) also triggers `wheels.yml`'s
`publish` job for that tag. Until then, the job's `environment: pypi` reference simply has
nowhere to authorize against and the workflow run for that job fails cleanly at the permission
check — the `build` job (and its artifact) is unaffected either way.

## Homebrew formula

A Homebrew formula for `ac3cli` is staged in-tree at
[`packaging/homebrew/Formula/ac3forge.rb`](https://github.com/iainchesworthlabs/ac3forge/blob/main/packaging/homebrew/Formula/ac3forge.rb)
and is pending publication to a personal tap (`homebrew-ac3forge`) - see
[`packaging/homebrew/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/packaging/homebrew/README.md)
for why a personal tap rather than a `homebrew-core` submission. Unlike the vcpkg port, this
packages the CLI (`ac3cli`), not the library: `AC3FORGE_BUILD_CLI=ON` with GUI/tests/examples/
fuzzers off, built from the release source tarball. The GUI (`ac3gui`) is not packaged here - a
Homebrew Cask, not a Formula, is the right shape for a bundled `.app`, and needs its own
follow-up.

**Every release tag** needs a follow-up update to the formula, same shape as the vcpkg port's:

1. Bump `packaging/homebrew/Formula/ac3forge.rb`'s `url` to the new tag and `sha256` to match
   (`sha256sum` the tag's release tarball - the same tarball the vcpkg port's `SHA512` already
   points at, just a different digest algorithm).
2. Validate locally first (see below) before touching a tap - a formula change that fails
   `brew audit` is slower to iterate on there than here.
3. Copy the updated formula into the `homebrew-ac3forge` tap's `Formula/ac3forge.rb` and push.

**Validating the formula locally**, from a macOS machine with Homebrew installed:

```bash
brew install --build-from-source ./packaging/homebrew/Formula/ac3forge.rb
brew test ac3forge
brew audit --formula ./packaging/homebrew/Formula/ac3forge.rb
brew uninstall ac3forge
```

There is no Homebrew on any of this project's CI runners or on Windows/Linux dev machines, so
this validation is manual and macOS-only - there is nothing here to automate against, unlike
the vcpkg `--overlay-ports` flow above.

## winget manifest

A winget manifest for `ac3forge` (`ac3cli` and `ac3gui` together, from the existing release
`.zip`) is staged in-tree at
[`packaging/winget/manifests/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/packaging/winget/manifests),
at the exact `manifests/<first-letter>/<publisher>/<package>/<version>/` path a
`microsoft/winget-pkgs` submission uses, so the version directory can be copied straight into a
fork of that repo. It uses `InstallerType: zip` with `NestedInstallerType: portable` against the
release's `ac3forge-X.Y.Z-win64.zip` rather than a dedicated installer - that archive already
contains both `bin/ac3cli.exe` and `bin/ac3gui.exe` (CPack's `runtime` component - see [What
gets published](#what-gets-published) below), and this release doesn't produce an NSIS `.exe`
(`makensis` wasn't on the runner - see `cmake/Packaging.cmake`). If a future release does
produce one, switch `InstallerType` to `nullsoft` and drop `NestedInstallerType`/
`NestedInstallerFiles` at that point rather than keeping both paths maintained.

**Every release tag** needs a new version directory, since winget-pkgs versions each release
independently rather than tracking a moving tag the way vcpkg's `version-semver` does:

1. Copy `packaging/winget/manifests/i/iainchesworthlabs/ac3forge/<prev-version>/` to a new
   `<new-version>/` directory, updating `PackageVersion` in all three files to match.
2. Update the installer manifest's `InstallerUrl` to the new release's `win64.zip` and
   `InstallerSha256` to match (`sha256sum` the zip - winget wants SHA256, unlike the
   `SHA512SUMS` `release.yml` publishes for every artifact, see [What gets
   published](#what-gets-published) below).
3. Validate locally first (see below) before touching a fork.
4. Copy the new version directory into the `microsoft/winget-pkgs` fork at the matching
   `manifests/i/iainchesworthlabs/ac3forge/<new-version>/` path and open the submission PR.

**Validating the manifest locally**, with the `winget` CLI (ships with Windows 10/11):

```bash
winget validate --manifest packaging/winget/manifests/i/iainchesworthlabs/ac3forge/<version>
```

## Conan recipe

A Conan (2.x) recipe for `ac3forge` is staged in-tree at
[`packaging/conan/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/packaging/conan)
(`conanfile.py`, `conandata.yml`, `test_package/`) and is pending submission to ConanCenter
(`conan-center-index`). Scoped the same as the vcpkg port - the library only (`ac3::forge`,
plus `matroska::matroska`/`mp4::mp4`/`mpegts::mpegts` behind their own default-on `matroska`/
`mp4`/`mpegts` options), never the CLI/GUI/tests/examples/fuzzers - with one Conan option per
`AC3FORGE_BUILD_<NAME>` CMake option, the same pattern the vcpkg port's `vcpkg_check_features()`
call already establishes. Rather than asking Conan's `CMakeDeps` generator to synthesise a
second CMake package config, the recipe sets `cmake_find_mode` to `"none"` and points consumers
at the config `cmake/InstallLibrary.cmake` already installs - see `conanfile.py`'s
`package_info()` comment. A consumer's `find_package(ac3forge CONFIG REQUIRED)` and
`target_link_libraries(main PRIVATE ac3::forge)` calls are identical to the vcpkg or plain
`cmake --install` case (see [docs/library/index.md](library/index.md)), Conan or not.

**Every release tag**, once the recipe has been merged upstream, needs a follow-up PR to
`conan-center-index` - ConanCenter has no mechanism to track a moving `main` either, same as
vcpkg's curated registry:

1. Add a new entry to `packaging/conan/conandata.yml`'s `sources` map, keyed by the new
   version, with the tag's release tarball `url` and `sha256` (same tarball the vcpkg port's
   `SHA512` and the Homebrew formula's `sha256` already point at, just fetched fresh).
2. Validate locally first (see below) before touching the upstream fork.
3. Copy the updated recipe into the `conan-center-index` fork's `recipes/ac3forge/`, add the
   new version to that recipe's own `config.yml`, and open the version-bump PR.

**Validating the recipe locally**, any time `packaging/conan/` or the CMake options it drives
change (whether or not a release is involved):

```bash
conan create packaging/conan --version <version> -s compiler.cppstd=23
conan create packaging/conan --version <version> -s compiler.cppstd=23 -o "&:shared=True"
conan create packaging/conan --version <version> -s compiler.cppstd=23 -o "&:matroska=False" -o "&:mp4=False" -o "&:mpegts=False"
```

`-s compiler.cppstd=23` is required - a bare default profile's `compiler.cppstd` predates
C++23 on most Conan installs, and `check_min_cppstd(self, 23)` in `conanfile.py` fails fast
rather than configuring a build that would fail deep inside compilation instead. Each command
above builds `test_package/`, which links `ac3::forge` and runs it, exercising the exact
`find_package(ac3forge)` path a real consumer uses - a passing `conan create` is a stronger
signal than a configure-only check for that reason. Fetching a real tag only exercises whatever
`AC3FORGE_BUILD_*` options actually existed in that tagged source, same caveat as the vcpkg
port's local-source-override technique above - `packaging/conan/conandata.yml` would need the
same scratch-entry treatment (a local `url` pointing at this checkout instead of a GitHub
tarball) to validate a CMake option added since the last tag.

## What gets published

One package per OS **and architecture**, not one per compiler-toolchain leg: `_build.yml`'s matrix
builds and tests both Windows toolchains (MSVC, clang-cl) and both Linux toolchains (GCC, Clang) - on
both x64 and arm64 for Linux - on every push, but only the leg marked `release_package: true` per
OS/arch actually packages for a release - windows-msvc, linux-gcc, linux-gcc-arm64 and macos-llvm.
windows-llvm, linux-llvm and linux-llvm-arm64 still catch compiler-specific bugs in full, every push;
they just don't produce a second, redundantly canonical archive that a downloader would have no way
to choose between. `cmake/Packaging.cmake` arch-qualifies the Linux archive filename
(`ac3forge-X.Y.Z-Linux-x86_64.tar.gz` vs. `...-Linux-aarch64.tar.gz`) specifically so the two Linux
architectures' TGZ/ZIP downloads never collide; DEB/RPM already carry their arch in their own
filenames.

| Platform | Arch | Leg | End-user packages | Library (`ac3forge-dev-*`) |
|---|---|---|---|---|
| Windows | x64 | windows-msvc | `.zip`, `.exe` (NSIS, if `makensis` is on the runner) | `.zip` |
| Linux | x86_64 | linux-gcc | `.tar.gz`, `.deb`, `.rpm` | `.tar.gz`, plus real system packages: `libac3forge0`/`ac3forge-devel` (RPM) and `libac3forge0`/`libac3forge-dev` (DEB) |
| Linux | aarch64 (Raspberry Pi 4/5 and other arm64 targets) | linux-gcc-arm64 | `.tar.gz`, `.deb`, `.rpm` | same split as x86_64, above |
| macOS | arm64 (Apple Silicon) | macos-llvm | `.tar.gz`, `.dmg` | `.tar.gz` |
| Android (Shield) | arm64 (NDK) | build-android | `.apk` | none - Shield links `ac3::forge`/`ac3::audio` in-tree, it isn't a `find_package(ac3forge)` consumer |

The end-user packages are `ac3cli`/`ac3gui` (CPack's `runtime` component) on desktop, or the
Shield app's `.apk` on Android. The library packages are a second, independent download for a
third party consuming `ac3::forge`/`matroska::matroska` via `find_package(ac3forge)` (see
[docs/library/index.md](library/index.md)) - headers, static and shared libraries, and the
CMake package config, but neither `ac3cli`/`ac3gui` nor `ac3::audio` (live capture/monitor/
passthrough stays a CLI/GUI-internal detail, not part of what's installed here).

Archive downloads (ZIP/TGZ) bundle everything above into one `ac3forge-dev-*` file, one per
platform regardless of compiler leg, same reasoning as the end-user package above - not NSIS (a
component installer can't also produce a second standalone download), not DragNDrop (no macOS
host to build or verify it against at all).

Linux additionally gets a **real runtime/`-dev` split** as proper system packages, not just an
archive: `cmake/InstallLibrary.cmake` files the shared libraries' versioned `.so` under its own
CPack component (`libruntime`, split out via `NAMELINK_COMPONENT` - see that file's comment),
separate from headers/static-archives/CMake-config/namelink-symlink (`library`). DEB/RPM's own
component-install switches turn that into three independent packages - `ac3forge` (the CLI/GUI),
`libac3forge0` (just the `.so` a linked binary loads), and `libac3forge-dev`/`ac3forge-devel`
(everything a builder needs, version-pinned to depend on the exact matching `libac3forge0`) -
the same `libFOO`/`libFOO-dev` shape as any other Linux C library, installable with a plain
`apt install`/`dnf install` rather than a manual archive download. ZIP/TGZ still produce one
merged `ac3forge-dev-*` archive as before (`cmake/Packaging.cmake` groups `library`+`libruntime`
together for the archive generators; `cmake/CPackProjectConfig.cmake` overrides that back apart
for DEB/RPM specifically). Confirmed against real `dpkg-deb -c`/`-I` and `rpm -qlp`/`-qRp` output
in a Linux build, not just a CMake reading - the pre-split `.deb` was, on inspection, a single
package silently bundling `ac3cli` together with the *entire* SDK (headers, static archives, and
the CMake package config all thrown in beside the binary), which this split also fixes as a
side effect.

The Shield `.apk` is signed with a real release keystore when one is provisioned (see
"Provisioning the Android release keystore" below), and falls back to AGP's default debug
keystore cleanly if it isn't - either way it's fine for sideloading onto a Shield in developer
mode. A release keystore is a prerequisite before this could ever go through the Play Store,
which sideloading itself doesn't require. (Not to be confused with **object signing** - the EMDF
Atmos authenticity tag, provisioned separately via the `ATMOS_SIGNING_KEY` secret and
unrelated to APK code-signing; see "Provisioning the Android object-signing key" below.)

No leg is `experimental: true` any more (see `ci.yml`'s status table), so all five package
for real rather than best-effort - a packaging failure on any of them blocks the release the
same as a build or test failure would. Every package - end-user or library - gets a `.sha512`
(`CPACK_PACKAGE_CHECKSUM` in `cmake/Packaging.cmake`), an aggregate `SHA512SUMS` manifest,
keyless Sigstore/OIDC build provenance, and an SPDX SBOM covering the whole release artifact
set - see Verifying a download below. GPG signatures are additional and only appear once a
signing key is provisioned (next section); their absence doesn't block a release.

## Provisioning the GPG signing key (optional, one-time)

GPG signing is off by default - the release workflow checks whether `REPO_GPG_PRIVATE_KEY` is
set and skips the signing steps cleanly if it isn't. **Nobody should ever paste a private key
into chat with an agent, or ask one to generate/handle key material** - do this yourself,
locally:

```bash
# 1. Generate a signing-only key (no passphrase keeps CI simplest - see the
#    tradeoff note below before deciding that's right for you).
gpg --batch --quick-generate-key "ac3forge <you@example.com>" rsa4096 sign never

# 2. Export the private key.
gpg --armor --export-secret-keys "ac3forge" > ac3forge-signing-key-private.asc
```

3. In the GitHub repo, go to Settings > Secrets and variables > Actions, and add:
   - `REPO_GPG_PRIVATE_KEY` - the full contents of `ac3forge-signing-key-private.asc`.
   - `REPO_GPG_PASSPHRASE` - only if you gave the key a passphrase in step 1.
4. Delete the local `ac3forge-signing-key-private.asc` file.

**The no-passphrase tradeoff**: a passphrase-less key is simpler to automate (no
`REPO_GPG_PASSPHRASE` secret, no interactive unlock to script around) but weaker if GitHub's
secret store is ever compromised - decide deliberately rather than defaulting to it. There is no
key-rotation procedure documented here; if you want one, design it before you need it, not
during an incident.

## Provisioning the Android release keystore (optional, one-time)

Off by default the same way GPG signing is - `_build.yml`'s `build-android` job checks whether
`ANDROID_KEYSTORE_BASE64` and its three companion secrets are set, and falls back to the debug
keystore cleanly if they aren't (see `build.gradle.kts`'s `releaseSigningAvailable`). **Nobody
should ever paste a private key into chat with an agent, or ask one to generate/handle key
material** - do this yourself, locally:

```bash
# 1. Generate a release keystore. Requires a JDK (keytool ships with any
#    JDK - `java -version` to check; CI uses Temurin 17, matching that isn't
#    required but keeps things consistent). Leave -storepass/-keypass off so
#    it prompts interactively - keeps the passwords out of shell history and
#    the process list. The -dname prompts (name/org/etc) only populate the
#    certificate's subject line, not security-relevant - answer them however
#    you like. PKCS12 (the modern default) uses one password for both the
#    keystore and the key - there is no separate key password to set.
keytool -genkeypair -v \
  -keystore ac3forge-shield-release.keystore \
  -storetype PKCS12 \
  -alias ac3forge-shield \
  -keyalg RSA -keysize 4096 \
  -validity 10000

# 2. Back up the keystore file itself right now, before doing anything else -
#    e.g. into a password manager's secure file storage, or an encrypted
#    offline drive. Unlike a GPG key, there is no separate keyring backing
#    this file up - it IS the private key, with no other copy anywhere.
#    Losing it means never being able to sign an update to this app under
#    the same identity again.

# 3. Base64-encode it into one line, ready to paste into a GitHub secret
#    (GitHub Actions secrets are text; this is the standard way to carry a
#    binary keystore through one).
base64 -w0 ac3forge-shield-release.keystore > ac3forge-shield-release.keystore.b64
```

4. In the GitHub repo, go to Settings > Secrets and variables > Actions, and add:
   - `ANDROID_KEYSTORE_BASE64` - the full contents of `ac3forge-shield-release.keystore.b64`.
   - `ANDROID_KEYSTORE_PASSWORD` - the password from step 1.
   - `ANDROID_KEY_ALIAS` - `ac3forge-shield` (or whatever `-alias` you used).
   - `ANDROID_KEY_PASSWORD` - the same password as `ANDROID_KEYSTORE_PASSWORD` (PKCS12 doesn't
     support a different one - see step 1).
5. Delete the local `ac3forge-shield-release.keystore.b64` file. **Keep the `.keystore` file
   itself** - see step 2.

No key-rotation procedure is documented here for the same reason as the GPG key above - design
one before an incident forces the question, not during it.

## Provisioning the Android object-signing key (optional, one-time)

Separate from the APK keystore above: this is the EMDF Atmos authenticity key that lets a
validating decoder reconstruct the objects (see [Object signing](concepts/object-signing.md)). Off
by default - `build-android` checks whether `ATMOS_SIGNING_KEY` is set and, if not, writes
no key asset, so the app ships the safe unsigned bed51 stream. **The key is yours to provision; do
not paste key material into a chat with an agent - do this yourself, locally.**

```bash
# Base64-encode your 32-byte key file into one line, ready to paste into a
# GitHub secret. CI writes this base64 verbatim into the app's bundled
# signing.key asset; the app base64-decodes it at startup (the same
# decode_signing_key() the desktop CLI uses, which also accepts a raw key).
base64 -w0 atmos.key > atmos.key.b64
```

Then, in the GitHub repo, go to Settings > Secrets and variables > Actions and add
`ATMOS_SIGNING_KEY` - the full contents of `atmos.key.b64` - and delete the local
`atmos.key.b64` afterward.

!!! danger "A signed APK carries the key"
    Because the app signs on-device, **any** Shield build with this secret set — the debug APK CI
    produces on every push included, not just releases — bundles the key as an asset. Treat every
    such APK as key material: it must never be distributed. A build without the secret is the safe
    unsigned app.

## Verifying a download

```bash
# Provenance (keyless, ties the bytes to this exact repo/workflow/commit)
gh attestation verify ac3forge-0.2.0-win64.zip --repo iainchesworthlabs/ac3forge

# GPG (ties the bytes to the maintainer's key, once one is provisioned)
gpg --import ac3forge-signing-key.asc
gpg --verify SHA512SUMS.asc SHA512SUMS && sha512sum -c SHA512SUMS
gpg --verify ac3forge-0.2.0-win64.zip.asc ac3forge-0.2.0-win64.zip
```

## Troubleshooting

**"... is not on main - releases must be cut from main"** - the commit you tagged (or the ref
you dispatched from) hasn't been merged to `main` yet.

**"tag vX.Y.Z already exists"** (manual dispatch only) - either retry with a different version,
or delete the existing tag first if it was created in error:
`git push origin :refs/tags/vX.Y.Z && git tag -d vX.Y.Z`.

**No package for a platform in the release** - that leg's `build-packages` job failed for real.
No leg is `experimental: true` any more (see [What gets published](#what-gets-published) above),
so a missing package is a genuine failure to investigate, not an expected gap for a
not-yet-promoted leg - check the run's `build-packages` job.

## What's deliberately not here

A tag-triggered release publishes signed, attested, SBOM'd packages and a GitHub Release. It does
**not** publish an APT/DNF package repository, a Docker image, or anything Home Assistant-shaped -
the earlier project this process was modelled on has release and repository-publishing workflows
to copy from if any of those are ever wanted here.
