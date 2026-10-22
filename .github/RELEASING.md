# Release process

Work lands in `develop` through reviewed PRs. A release branch starts at the exact
`origin/develop` commit, records its provenance in `.github/release.json`, and
merges into `main` through a reviewed PR. The Release workflow checks that merge,
runs Build against it, then creates the tag and branded GitHub release.

The first `0.1.0` release requires completed M2 and M3 milestones, an accepted real
two-deck mixing trial, and a committed record of that evidence. M2's Best-quality
live-stem latency must remain below **4.095 seconds at p99**. This development
release remains separate from M6's full first-usable-release scope in the
[development plan](../docs/handoff/PLAN.md#implementation-sequence-and-exit-criteria).
Neither milestone is complete yet; no release branch has been prepared.

## Versioning

Use `release/X.Y.Z` branches and `vX.Y.Z` tags. The initial release is explicitly
`0.1.0`. After that, conventional commits since the preceding release determine
the minimum version increase:

| Commit | Minimum increase |
| --- | --- |
| `fix`, `perf`, `revert` | Patch |
| `feat` | Minor |
| `!`, `BREAKING CHANGE:` or `BREAKING-CHANGE:` | Major, including during `0.x` |
| `docs`, `ci`, `test`, `chore`, `refactor`, `style` alone | No release |

Breaking changes take precedence over type. Higher versions may mark an
intentional milestone. Never move or reuse a published version tag for a
different commit. `cliff.toml` supplies the grouped conventional-commit changes.
Nonbreaking documentation and maintenance commits are omitted, along with merge
commits, so release notes focus on releasable changes and list merged work once.

## Prepare a release

Merge the intended changes and the acceptance record into `develop` first.
The record must be a tracked, nonsymlink Markdown file at the recorded develop
commit. Add `<!-- conflux-mixing-accepted -->` on its own line only after the user
accepts a real mixing trial and the relevant functionality tests pass. For
`0.1.0`, the same record must also contain `<!-- conflux-m2-complete -->` and
`<!-- conflux-m3-complete -->`, each on its own line. Add those declarations only
after each milestone's required evidence passes and is recorded. Link that
evidence in the record; the declarations do not waive latency, musical or
interaction checks. Record unavailable interface or headphone-isolation checks
explicitly. The helper requires prose beyond the declarations and preserves the
record's exact bytes through the release and main merge.

Install git-cliff **2.14.2**, matching CI. From a clean working tree, replace the
evidence path below with the actual committed record:

```sh
python3 .github/scripts/release.py prepare 0.1.0 \
  --mixing-evidence 'docs/<actual-accepted-mixing-record>.md'
```

The helper fetches `origin/develop`, `origin/main` and tags, creates
`release/0.1.0` from the recorded develop commit, and updates `CMakeLists.txt`,
`CHANGELOG.md` and `.github/release.json`. It does not commit automatically.
For later releases, supply the next version and the evidence required by the
release's scope. Review the generated changes before staging:

```sh
git add CMakeLists.txt CHANGELOG.md .github/release.json
git commit -S -m 'chore(release): prepare 0.1.0' \
  -m 'Record M2 and M3 completion, the accepted two-deck mixing trial and exact develop source. Align the application version and generated changelog for the reviewed main release.'
git push -u origin release/0.1.0
```

`kyau` authors and signs the commit with the existing hooks enabled.
`kyaulabs-bot` opens the same-repository PR from `release/0.1.0` into `main`
using [the PR template](PULL_REQUEST_TEMPLATE.md). Complete its Test Plan after
CI passes, obtain `kyau` approval with `✔️ Approved by: @kyau`, and merge as
`kyaulabs-bot` using a merge commit. Squash and rebase do not preserve the
release merge checked by the workflow.

## Publication and back-merge

Release accepts a merged, same-repository `release/X.Y.Z` PR into `main`.
It validates the recorded source, version floor, version metadata and merge
identity, then runs the existing Build workflow on the exact merged commit.
The tag and GitHub release use that commit. Notes combine
[the CONFLUX release template](RELEASE_TEMPLATE.md), git-cliff changes and the
recorded verification evidence. The supplied brand banners are pinned to the
release tag. GitHub provides source archives. Publication does not deploy the
application or distribute model weights, music or private data.

The separate [Release backmerge workflow](workflows/backmerge.yml) runs only
after Release succeeds. It verifies the published tag and upstream release,
verifies the bot identity using
the existing organization `BACKMERGE_TOKEN`, and opens a direct `main` to
`develop` PR with the repository template. No new secret is required for this
setup. The workflow does not approve or merge that PR.

Complete the back-merge Test Plan, wait for its normal CI, obtain `kyau` approval
with `✔️ Approved by: @kyau`, and merge as `kyaulabs-bot`. Resolve conflicts in
the reviewed PR. Keep `main`, `develop` and `release/X.Y.Z`; the back-merge uses
`main` directly, so it creates no temporary branch to delete. Branch protection,
signing and hook rules remain in force.

## Recover a failed workflow

Inspect the failed job and fix its reported cause. Rerun the failed jobs, or
dispatch Release with the original, already-merged release PR number:

```sh
gh workflow run release.yml --ref main -f release_pr=123
```

Dispatch retries the validated release; it cannot publish an arbitrary commit,
an unmerged PR or a fork PR. Matching tags and releases may be reused for that
same commit. Do not delete or move a published tag to retry. A successful Release
run can trigger the back-merge workflow; verify its PR and checks before calling
the release process complete.
