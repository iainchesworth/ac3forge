# Releasing ac3forge

How to cut a release: what triggers `.github/workflows/release.yml`, what it produces, and how
to set up the optional GPG signing key. Modelled on `R:\aqualink-automate`'s
`docs/releasing.md`, with the parts that don't apply to ac3forge (APT/DNF repository
publishing, a Docker image, a Home Assistant add-on) removed.

## Versioning

ac3forge does **not** derive its version from git tags the way aqualink-automate does.
`project(ac3forge VERSION ...)` in the top-level `CMakeLists.txt` is the source of truth - it's
what actually gets baked into every built package (see `cmake/Packaging.cmake`'s
`CPACK_PACKAGE_VERSION_MAJOR`/`MINOR`/`PATCH`) and into `ac3cli --version`'s `version_string`.

What the release workflow enforces instead: the tag you push must match `CMakeLists.txt`'s
`VERSION` exactly, or `resolve-version` fails before anything builds. So the order is always:

1. Bump `CMakeLists.txt`'s `project(... VERSION X.Y.Z ...)`.
2. Bump `vcpkg.json`'s `"version"` to match (see the comment beside it - nothing keeps these
   two in sync automatically).
3. Merge to `main`.
4. Tag.

Tags are strict SemVer 2.0.0: `vMAJOR.MINOR.PATCH[-(alpha|beta|rc).N]`, e.g. `v0.2.0` or
`v0.2.0-beta.1`. A tag with a prerelease suffix (or the dispatch form's `prerelease` checkbox)
marks the GitHub Release as a prerelease; the bare `X.Y.Z` still has to match `CMakeLists.txt`
either way - the suffix is a release-level label CMake's `VERSION` field can't hold, not a
different version.

## Pre-release checklist

1. CI green on `main` for the commit you're about to tag.
2. Releases must be **cut from main** - `resolve-version` checks this with
   `git merge-base --is-ancestor` and fails otherwise (dry runs are exempt).
3. `CMakeLists.txt`'s `VERSION` and `vcpkg.json`'s `"version"` both already match the tag you're
   about to push (see Versioning above) - `resolve-version` checks the first but not the second.
4. Decide the tag.

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
previous tag - a first draft, not a finished changelog. Curate it:

```bash
gh release edit vX.Y.Z --notes-file notes.md
```

## What gets published

| Platform | Packages | Built for real? |
|---|---|---|
| Windows (MSVC / clang-cl) | `.zip`, `.exe` (NSIS, if `makensis` is on the runner) | windows-msvc yes; windows-llvm best-effort |
| Linux (GCC / clang) | `.tar.gz`, `.deb`, `.rpm` | best-effort (`experimental: true` legs) |
| macOS (AppleClang) | `.tar.gz`, `.dmg` | best-effort, unverified - no Mac host locally |

"Best-effort" means the leg runs `continue-on-error` in `_build.yml` - if it fails to even
compile, the release simply ships without that platform's package rather than failing
outright. Every package that IS built also gets a `.sha512` (`CPACK_PACKAGE_CHECKSUM` in
`cmake/Packaging.cmake`), an aggregate `SHA512SUMS` manifest, keyless Sigstore/OIDC build
provenance, and an SPDX SBOM covering the whole release artifact set - see Verifying a download
below. GPG signatures are additional and only appear once a signing key is provisioned (next
section); their absence doesn't block a release.

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

**"tag vX.Y.Z (version X.Y.Z) does not match CMakeLists.txt's project() VERSION ..."** - you
tagged before bumping `CMakeLists.txt`. Delete the tag (`git push origin :refs/tags/vX.Y.Z && git
tag -d vX.Y.Z`), bump `CMakeLists.txt` and `vcpkg.json`, merge to main, retag.

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
