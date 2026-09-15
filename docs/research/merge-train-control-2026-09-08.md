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
