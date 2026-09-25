<!-- markdownlint-disable MD013 -->
# Merge-train control investigation, 2026-09-08

The local train promoted stacked PR #1420 and merged it into #1396 after a rebase
push was rejected. The draft selector checked holds; the ready, explicit rebase,
and agent-operator paths did not. PR inventory omitted `baseRefName`, and the
rebase helper reused a registered agent checkout. Its error status did not stop
the subsequent `gh pr ready` / `gh pr merge --squash --auto` sequence.

The observed merge commit was `834f84063a4640e72b0916a9a5b3a566b6810fec`, recorded
at `2026-09-08T16:16:50Z` on [PR #1420](https://github.com/VMAFx/vmafx/pull/1420).
Local script snapshots and process suspension receipts are retained in the RC1
initiative evidence (`2026-09-08-rc1-hygiene/train-race`), separate from source.

Installed GitHub CLI help verifies `--match-head-commit`, `--required`, required
check buckets, and the distinct exit 8 for pending checks. Installed Git help
verifies detached worktrees and explicit force-with-lease expectations. GNU Make
4.4.1 supports environment recipe/dry-run controls; clearing only `MAKEFLAGS`
does not clear `MAKEFILES`. An ignored `GNUmakefile` also shadows `Makefile`, so a
clean Git status alone is insufficient to identify the actual gate entry point.

The regression suite runs real Git rebases/pushes against a disposable local bare
remote and actual `make lint` / `make test` fixture recipes. GitHub responses are
fixtures. Conflict and push-rejection cases assert that no ready/merge call
occurs; hold/base/owner checks run across all mutation entry points. Receipt
tests execute both recipes, reject altered logs/receipts and ignored Makefile
shadowing, and preserve a revoked earlier receipt when a repeat test fails.

The chosen boundary is a shared tracked gateway plus explicit runtime migration,
as recorded in [ADR-1244](../adr/1244-merge-train-ownership-and-validation.md).
Duplicated draft-only filters cannot protect ready/rebase/operator paths.
GitHub branch protection cannot protect a local worktree and may not exist on a
stacked branch. Full VMAFx local gates remain required before merge; this control
test suite neither substitutes for those gates nor establishes RC1 readiness.

## Runtime migration verification, 2026-09-25

The local merge-train runtime migration was independently verified and closed on
2026-09-25:

- Installed runtime adapters in `/home/kilian/dev/vmafx/vmafx/.claude/mergetrain`
  (`train.sh`, `rebase-clean.sh`, `watchdog.sh`, `merge_train_operator.py`) are
  hash-bound to committed gateway
  `a7a58dd8f39576dc2b0a86fb5af518003496b147261dc5294706cbce976f39c7`
  (commit `9bae48c2c214d189a8df5602ac38c94f3997f234`, blob
  `4ca44da40edfc03fc500174a193b293ca8cfdc3e`), matching `releases/a7a58dd8.../merge_train_guard.py`.
- The live installation is backed by `migration-xghk30zh` (`plan.json`,
  `receipt.json`, hash `94720ca125fc500e3d844ae927ed25e06532ee6c36608895d7172a40f121ac5b`)
  and the initial `migration-6wtg48lv`.
- Legacy unrestricted VMAFx actors are absent. Any active process with a
  matching name (`PID 3790561 /usr/bin/bash ./train.sh`) belongs to its actual
  external working directory (`/home/kilian/dev/scratch/k8s-worktrees/.mergetrain`).
- Held PRs, non-master bases, and worktree ownership fail closed across all actions;
  release PR #1213 is excluded; and required checks and exact-head validation
  receipts fail closed.
- The 26-test disposable regression suite
  (`python3 -m unittest discover -s scripts/dev/tests -p 'test_*merge_train_guard.py'`)
  passes completely.
