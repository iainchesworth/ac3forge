# Releasing ac3forge

How to cut a release: what triggers `.github/workflows/release.yml`, what it produces, and how
to set up the optional GPG signing key. Modelled on `R:\aqualink-automate`'s
`docs/releasing.md`, with the parts that don't apply to ac3forge (APT/DNF repository
publishing, a Docker image, a Home Assistant add-on) removed.

## Versioning

ac3forge derives its version from git tags, the same way aqualink-automate does.
`cmake/GitVersionDerivation.cmake` runs `git describe --tags --match "v*"` **before**
`project()` in the top-level `CMakeLists.txt` and feeds the result straight into
`project(ac3forge VERSION ...)` - the tag is the single source of truth. Nothing in the tree
hardcodes a version to bump by hand: not `CMakeLists.txt`, and not `vcpkg.json`'s `"version"`
field, which is a fixed placeholder never read for anything but satisfying vcpkg's manifest
schema (see the comment beside it).

So the order is just:

1. Merge to `main`.
2. Tag.

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

Builds and packages every platform (best-effort - see `_build.yml`'s `experimental` legs)
without tagging, signing, or publishing anything. Exempt from the cut-from-main guard, so it can
run from any branch - use it to validate a packaging change before merging.

## Post-release

`gh release create --generate-notes` drafts release notes from merged PRs/commits since the
previous tag - a first draft only. Modelled on aqualink-automate's own process
(`R:\aqualink-automate\docs\releasing.md`), curating it to the established pattern is a
required step, not optional polish:

1. Update [CHANGELOG.md](https://github.com/iainchesworth/ac3forge/blob/main/CHANGELOG.md) first, if it isn't already current - a `## [x.y.z] -
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
      open gap (see [Known gaps](https://github.com/iainchesworth/ac3forge/blob/main/CHANGELOG.md#known-gaps) in the matching CHANGELOG.md
      section).
3. Apply it:

   ```bash
   gh release edit vX.Y.Z[-beta.N] --notes-file notes.md
   ```

4. Verify the release page has all expected artifacts, and that the curated notes render and
   read well.

## What gets published

One package per OS, not one per compiler-toolchain leg: `_build.yml`'s matrix builds and tests
both Windows toolchains (MSVC, clang-cl) and both Linux toolchains (GCC, Clang) on every push,
but only the leg marked `release_package: true` per OS actually packages for a release -
windows-msvc, linux-gcc and macos-llvm. windows-llvm and linux-llvm still catch
compiler-specific bugs in full, every push; they just don't produce a second, redundantly
canonical zip that a downloader would have no way to choose between.

| Platform | Leg | End-user packages | Library (`ac3forge-dev-*`) |
|---|---|---|---|
| Windows | windows-msvc | `.zip`, `.exe` (NSIS, if `makensis` is on the runner) | `.zip` |
| Linux | linux-gcc | `.tar.gz`, `.deb`, `.rpm` | `.tar.gz` |
| macOS | macos-llvm | `.tar.gz`, `.dmg` | `.tar.gz` |
| Android (Shield) | build-android | `.apk` | none - Shield links `ac3::forge`/`ac3::audio` in-tree, it isn't a `find_package(ac3forge)` consumer |

The end-user packages are `ac3cli`/`ac3gui` (CPack's `runtime` component) on desktop, or the
Shield app's `.apk` on Android. The library packages are a second, independent download for a
third party consuming `ac3::forge`/`matroska::matroska` via `find_package(ac3forge)` (see
[docs/library/index.md](library/index.md)) - headers, static and shared libraries, and the
CMake package config, but neither `ac3cli`/`ac3gui` nor `ac3::audio` (live capture/monitor/
passthrough stays a CLI/GUI-internal detail, not part of what's installed here). Archive-only
(ZIP/TGZ) for now, one per platform regardless of compiler leg, same reasoning as the end-user
package above - not NSIS (a component installer can't also produce a second standalone
download), not DEB/RPM (a correct runtime/`-dev` split needs per-component `Depends` metadata,
a separate initiative if ever wanted), not DragNDrop (no macOS host to build or verify it
against at all).

The Shield `.apk` is signed with a real release keystore when one is provisioned (see
"Provisioning the Android release keystore" below), and falls back to AGP's default debug
keystore cleanly if it isn't - either way it's fine for sideloading onto a Shield in developer
mode. A release keystore is a prerequisite before this could ever go through the Play Store,
which sideloading itself doesn't require. (Not to be confused with
`platform/android/app/build.gradle.kts`'s `quarantineSignerEnabled` - the project's own
EMDF/Atmos authenticity signer, unrelated to APK code-signing.)

No leg is `experimental: true` any more (see `ci.yml`'s status table), so all four package
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

## Verifying a download

```bash
# Provenance (keyless, ties the bytes to this exact repo/workflow/commit)
gh attestation verify ac3forge-0.2.0-win64.zip --repo iainchesworth/ac3forge

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

**No package for a platform in the release** - that leg's `experimental: true` build failed
(check the run's `build-packages` job); expected until that leg is promoted out of experimental
in `_build.yml` - see the table in `ci.yml`.

## What's deliberately not here

A tag-triggered release publishes signed, attested, SBOM'd packages and a GitHub Release. It does
**not** publish an APT/DNF package repository, a Docker image, or anything Home Assistant-shaped -
`R:\aqualink-automate`'s `.github/workflows/release.yml` / `publish-repos.yml` and
`docs/releasing.md` are the precedent to copy from if any of those are ever wanted here.
