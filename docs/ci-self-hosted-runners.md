# Self-hosted CI runners

The four plain Windows/Linux legs in [`_build.yml`](https://github.com/iainchesworth/ac3forge/blob/main/.github/workflows/_build.yml)'s
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
   - This repo's own registered runners (`GET /repos/iainchesworth/ac3forge/actions/runners`,
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
