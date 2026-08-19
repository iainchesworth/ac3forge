# Self-hosted CI runners

The four plain Windows/Linux legs in [`_build.yml`](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/workflows/_build.yml)'s
`build` matrix (Windows MSVC, Windows LLVM, Linux GCC, Linux LLVM) can run on a self-hosted
runner instead of a GitHub-hosted one - but only when one is actually online and idle right
now, never as an all-or-nothing switch. macOS and the two arm64 Linux legs always stay on
GitHub-hosted runners; there's no self-hosted equivalent for either.

This page describes what ac3forge's CI does with a self-hosted runner once one exists. It
does not describe how one comes to exist - the fleet itself (Packer images, provisioning
scripts, the org they register against) lives in
[iainchesworthlabs/ci-runners](https://github.com/iainchesworthlabs/ci-runners), a repo
shared across every project in the `iainchesworthlabs` organization rather than owned by
this one.

## How the decision gets made

A `check-runners` job runs before the `build` matrix on every push/PR/release, decides a
runner-label set for Linux and one for Windows, and the matrix picks those up via
`runs-on: ${{ fromJSON(matrix.runner) }}`. Per OS, in order:

1. **Fork PRs always get GitHub-hosted**, no exceptions and no live check. Untrusted code
   must never land on self-hosted infrastructure - the runners are ephemeral (wiped between
   every job) but that only bounds damage *between* jobs, not *during* one.
2. **An explicit override wins next.** Repository variables `RUNNER_LINUX_MODE` and
   `RUNNER_WINDOWS_MODE` accept `auto` (the default, used whenever the variable is unset),
   `self-hosted`, or `github-hosted`. A forced mode skips the live check entirely - if you
   force `self-hosted` and nothing is actually online, the job queues and waits, which is
   the expected cost of an explicit override.
3. **`auto` runs a live check**, in two parts, either one sufficient:
   - This repo's own registered runners (`GET /repos/iainchesworthlabs/ac3forge/actions/runners`,
     using the workflow's own `GITHUB_TOKEN` - no extra setup). Empty until a runner is
     actually registered directly against this repo, which may never happen under the
     org-level model `ci-runners` uses.
   - `iainchesworthlabs`'s org-level runners (`GET /orgs/iainchesworthlabs/actions/runners`),
     only attempted if the optional repository secret `ORG_RUNNERS_TOKEN` is set - an
     org-scoped PAT or GitHub App token with "Self-hosted runners: read". This is what
     actually starts mattering once the org migration and runner group in `ci-runners` are
     in place; until then it's simply skipped, not an error.

   Either check finding at least one runner that is `online`, not `busy`, and labelled with
   both `self-hosted` and the right OS (`Linux` or `Windows`) selects the self-hosted label
   set (`self-hosted, Linux, X64` / `self-hosted, Windows, X64` - the exact labels the
   `ci-runners` fleet registers with). Finding nothing, or the API call itself failing for
   any reason, falls back to GitHub-hosted (`ubuntu-latest` / `windows-latest`) - this check
   being unavailable is never a reason to block CI.

Today, before any runner is registered against ac3forge under either model, every leg simply
keeps building on GitHub-hosted runners - a deliberate no-op, not a bug: the mechanism is
inert until the infrastructure side catches up.

## Why the check, not a static switch

An earlier, simpler design (a single repository variable holding the literal runner-label
array, the pattern `aqualink-automate` currently uses) only answers "has someone configured
self-hosted for this leg", not "is a self-hosted runner actually able to pick this job up
right now". With `ci-runners`' runners shared across every repo in the org, "configured" and
"available" can genuinely differ moment to moment, so the live check is what keeps a leg from
silently queuing behind another repo's job instead of falling back.

## Measuring whether it's actually worth it

The whole point of this is to find out whether self-hosted is meaningfully faster than
GitHub-hosted for ac3forge's build - not to assume it. Nothing here pre-bakes toolchains onto
the self-hosted image or skips any install step; every leg installs GCC/LLVM/Qt/ffmpeg/Ninja
and warms vcpkg's cache identically regardless of which runner it landed on, so a timing
comparison between the two is measuring the runner, not a shortcut. Pre-baking is a
reasonable follow-up once there's real data to justify it - not built in advance of that data.

## Toolchain version pins

A runner (self-hosted or GitHub-hosted) picking up a job is one problem; that runner actually
having the *right versions* of the compiler, CMake, Qt, and vcpkg once it does is a separate
one. ac3forge ports the manifest-driven version check
[`aqualink-automate`](https://github.com/iainchesworthlabs/aqualink-automate) and
[`ci-runners`](https://github.com/iainchesworthlabs/ci-runners) both use, adapted to this
repo's own architecture (see [Why no separate drift-warning action](#why-no-separate-drift-warning-action)
below for what differs and why).

1. **One manifest, several sources.**
   [`.github/toolchain-versions.json`](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/toolchain-versions.json)
   holds only the two pins that have no other canonical home in this repo: the MSVC toolset
   prefix (`msvc_toolset`) and the Qt version (`qt`). Everything else this repo already pins
   somewhere is read from that real location instead of being duplicated into the manifest
   too: GCC/LLVM majors come from `.github/toolchain/02-gcc-toolchain.sh` and
   `03-llvm-toolchain.sh` (the scripts that actually install them - copied verbatim from
   `aqualink-automate` so both projects track the same toolchain), CMake's minimum from
   `CMakePresets.json`'s `cmakeMinimumRequired`, and vcpkg's baseline from `vcpkg.json`'s
   `builtin-baseline`.
   [`_toolchain-versions.yml`](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/workflows/_toolchain-versions.yml)
   is a small reusable `workflow_call` (same shape as the `check-runners` job above) that
   reads the manifest plus those four other files once and exposes `gcc_version` / `llvm_version` / `msvc_toolset`
   / `qt_version` / `cmake_min` / `vcpkg_commit` as outputs. Every workflow that used to
   hardcode a `vcpkg-commit: "eaca4a5..."` or `version: "6.8.3"` literal - `_build.yml`,
   `ci.yml`, `msvc-analysis.yml`, `fuzz.yml`, `codeql.yml` - instead calls it as its own
   `toolchain-versions` job and reads `needs.toolchain-versions.outputs.*`. Before this,
   the vcpkg commit alone was hardcoded independently in nine separate places across five
   workflow files; bumping it meant finding and editing all nine by hand, with no error if
   one was missed. **Bump a version by editing whichever of the four files actually owns
   it** - nothing else in this repo needs to change.
2. **Runtime assert, on every leg, both runner paths.** `_build.yml`'s existing "Report and
   assert toolchain versions" step (unchanged in spirit, now manifest-driven) runs `g++`/
   `clang++`/`clang-cl --version` or reads the MSVC environment's `VCToolsVersion`, compares
   it against the matching `needs.toolchain-versions.outputs.*` pin, and fails the job with
   an `::error::` annotation on a mismatch. It runs identically whether the leg landed on a
   self-hosted or GitHub-hosted runner - see the next section for why that is the right
   severity here, unlike the warn-only check `aqualink-automate` uses.

### Why no separate drift-warning action

`aqualink-automate` pairs its manifest with a second mechanism,
[`check-toolchain-drift`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/.github/actions/check-toolchain-drift/action.yml):
a composite action that runs *only* on a self-hosted job, compares the installed compiler
against the pin, and **warns** rather than fails. That shape fits `aqualink-automate` because
its self-hosted legs *skip* the fresh-install step entirely and build straight against
whatever `ci-runners`' Packer image happened to bake in - a mismatch there means the shared,
externally-provisioned image has drifted, which is `ci-runners`' problem to fix, not a reason
to block a PR against unrelated code.

ac3forge's own design already differs in the one place that matters: every Linux leg runs
inside a pinned `ubuntu:26.04` **container** (see `build`'s own comment in `_build.yml`), so
GCC/LLVM/Qt/ffmpeg/Ninja are installed fresh into that container on *every* run, self-hosted
or GitHub-hosted alike - the host image's own toolchain, pre-baked or not, is never reachable
from inside it. There is no self-hosted-only drift path for those tools to warn about; a
mismatch there can only mean this repo's own install step is broken, on both runner types
equally, which is exactly what should fail the build immediately. The one leg that does
depend on whatever the host actually has - Windows MSVC, which this workflow never installs,
only asserts - already fails on both runner paths today, which is the stricter, appropriate
choice for a leg whose bit-exact gold-reference output can depend on the exact toolset. So the
manifest above is ported in full; the separate warn-only self-hosted action is deliberately
not, because there is no scenario in this repo where softening the check for self-hosted
specifically would be correct. If toolchain pre-baking is ever added per the note above, this
call is worth revisiting.
