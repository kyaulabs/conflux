# 🤖 AGENTS.md

## Documentation

- All major documentation requires the `distill` skill
- When modifying `README.md` files, do not touch layout, logos, primary
  headers or any shields.io badges unless explicitly instructed

## Git

- All commits require GPG signature
- All commits must follow conventional commit syntax configured in this repo
- All commits must have detailed body messages
- Never commit/push directly to `main` or `develop`, create branches
  `<type>/kyau-(openssl rand -hex 3)-name-of-branch` where `<type>` is
  conventional commits type
- GitHub user `kyau` is authenticated via Yubikey to SSH/GPG for `git`
- GitHub users `kyau` and `kyaulabs-bot` are authenticated via `gh`
- All commits/reviews are to be done by `kyau`.
- All PRs must created/merged with the supplied template by `kyaulabs-bot`
- Once a PR is created the `Test Plan` section needs to be completed in full
  before `kyau` approves the review
- Approve reviews with `✔️ Approved by: @kyau`
- Watch all workflows, if they fail debug and fix automatically and then re-run
- Run all git commands automatically, approval granted
- Delete all stale branches with `kyaulabs-bot` (NO `main`, `develop`, or
  `release/X.Y.Z`)
- All local rulesets `ruleset-*.json` should be respected for private
  repositories
- NEVER bypass hooks or disable Gitleaks/commtlint to force a commit through

## Semantics

- Do not use sub-titles when just a title will do
- Do not insert random sub-titles unless asked to do so
- Simple is always better provided it gets the job done adaquetely

## Scope Guard

Complete the task with the smallest sufficient change.
Explicit user instructions for this task override the defaults below.

### Before editing

- Read relevant code, call paths, and project conventions. Do not explore
  the entire repository before a small change.
- Make clear, small fixes directly. Write a short plan when the approach
  is unclear or the impact is substantial.
- Decide routine details yourself. Ask when different interpretations
  would lead to materially different work.
- Load only relevant Skills, not an entire workflow for a matching keyword.

### While editing

- Before adding code, check in this order:
  1. Existing code and patterns in the project
  2. Standard library and built-in platform capabilities
  3. Dependencies already installed
  4. Only then, new code required for the current task
- Fix the root cause. Do not add abstractions, configuration, or
  compatibility layers for hypothetical future needs.
- Justify new dependencies or frameworks: why are existing options not enough?
- Fix nearby issues only if they block the task. Otherwise, flag them
  for follow-up.
- Prefer precise edits for small changes. Avoid unrelated refactoring
  or whole-file rewrites.
- Remove replaced implementations. Keep old paths only when compatibility
  is explicitly required.
- Preserve necessary validation, error handling, security, and accessibility.

### When to ask

- Keep implementing, verifying, and fixing within the authorized scope.
  Do not repeatedly ask whether to continue.
- Confirm material scope expansion, unapproved costs or permissions,
  and unauthorized irreversible actions before proceeding.
- If asked only to analyze or review, report findings. Do not edit code.

### Testing

- Verify the change and its impact. Complete required project checks.
- Reuse existing tests first. Add tests for real behavior and regression
  risks, not tests that mechanically repeat the implementation.
- Temporary verification scripts need not become permanent test files.
- Once checks pass, broaden or repeat them only for new changes,
  failures, or specific unresolved concerns.
- Do not write tests for reversible, low-impact changes that mirror the
  implementation. Make sure that the tests are meaningful and necessary
  to verify implementation.

### If the plan grows

If you find future-only layers, unrelated refactoring, extra features,
or unjustified repeat checks, return to the current requirement.
Drop the excess work and finish within the existing authorization.

### Done means

- The requested behavior works and necessary verification is complete.
  Do not stop at a plan or an initial implementation.
- Every change serves the task. Remove obsolete code and temporary files
  introduced by this work.
- Briefly report the result, verification evidence, and anything
  unresolved or unverified.
