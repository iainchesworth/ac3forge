# Branch protection for `main` and `develop`

*Maintainer notes — repo administration; not published on the docs site.*

GitHub branch/repo security settings can't be expressed as a workflow file -
they're applied in **Settings → Branches** (or **Settings → Rules → Rulesets**)
by someone with admin rights on the repo. Configure a protection rule (or
ruleset) for `main` with:

- **Require a pull request before merging**
  - Require at least 1 approval
  - Dismiss stale approvals when new commits are pushed
- **Require status checks to pass before merging** (enable "Require branches
  to be up to date" too), selecting:
  - `Branch Name` (from `ci.yml`)
  - `No Quarantine On Main` (from `ci.yml`)
  - `CI Status` (from `ci.yml` - aggregates every required CI job; add or
    rename a matrix leg without ever touching this rule)
  - `Analyze (C++)` (from `codeql.yml`)
  - `Scan dependency diff` (from `dependency-review.yml`) - fails on a
    moderate-or-worse known vulnerability newly introduced by the PR
    (`vcpkg.json` or a GitHub Actions dependency)
- **Require conversation resolution before merging**
- **Do not allow bypassing the above settings** (applies rules to admins too)
- **Restrict who can push to matching branches** - only allow merges via PR;
  block direct pushes
- **Block force pushes**
- **Restrict deletions**

`develop` is where `feature/*`/`bugfix/*` work actually lands and where
Dependabot opens its PRs (`.github/dependabot.yml` targets `develop`, not
`main`), so give it the same rule with the same required checks - that's
where most vulnerable dependencies or CI regressions would actually be
introduced, well before a release PR ever reaches `main`.

Since `main` only receives merges from `develop`, `release/*`, `hotfix/*` and
`support/*` branches under this project's gitflow model (see
`CONTRIBUTING.md` and the `branch-name` job in `ci.yml`), you may also want a
rule restricting which branches can open PRs against `main` - GitHub
rulesets support this directly (`main` ruleset → target branch pattern
restrictions), whereas classic branch protection does not; the `branch-name`
job enforces the naming convention as a required status check either way.

## Other scanners (visible-only)

`osv-scanner.yml`, `zizmor.yml` and `scorecard.yml` upload SARIF to
**Security → Code scanning** but don't fail PR checks - triage their alerts
there rather than via a required status check (see each workflow's header
comment for why). `msvc-analysis.yml` (MSVC Code Analysis, `/analyze`) is
the same shape and runs on PRs to `main`/`develop` too (docs-only changes
skipped): its findings land in code scanning for triage, not in a required
status check. `scorecard.yml`'s branch-protection sub-check scores more
completely with a fine-grained PAT (read-only, "Administration: read") added
as a repo secret named `SCORECARD_READ_TOKEN`; without it, that one
sub-check just degrades gracefully instead of failing.

## Dependabot auto-merge

`dependabot-auto-merge.yml` only flips the auto-merge bit on a Dependabot PR
(non-major bumps only); GitHub still won't merge it until every required
check above passes. It needs no extra configuration beyond the branch
protection rule itself - once `Branch Name`, `CI Status` and
`Scan dependency diff` are required on `develop`, auto-merge is safe to
enable repo-wide in **Settings → General → Pull Requests → Allow auto-merge**.
