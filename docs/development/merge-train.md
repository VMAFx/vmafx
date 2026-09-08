<!-- markdownlint-disable MD013 -->
# Local merge-train control

`scripts/dev/merge_train_guard.py` controls the local VMAFx queue. It accepts only
open, same-repository PRs targeting `master`, excludes release PR #1213, and
checks holds and worktree ownership before every mutation. It never edits an
existing source checkout. The rationale is [ADR-1244](../adr/1244-merge-train-ownership-and-validation.md).

## Inspect and configure

Use Python 3.10 or newer on POSIX, Git, authenticated GitHub CLI, and the existing
full-gate toolchain. The installed GitHub CLI must support `pr merge
--match-head-commit` and `pr checks --required --json`. GitHub requests name
`github.com/VMAFx/vmafx` explicitly. Both origin fetch and push URLs must identify
only the canonical repository; multiple push destinations are refused.

Keep mutable operator state outside tracked source, normally in
`.claude/mergetrain/`. A missing or malformed policy/hold file fails closed.
Create `policy.json` with the current owners, for example:

```json
{
  "schema": 1,
  "protected_branches": ["build/base-image-single-source", "build/version-single-source-tree"],
  "protected_worktrees": ["/home/kilian/dev/vmaf/.claude/worktrees/agent-rc1-ffmpeg"]
}
```

`hold.txt` contains one PR number per line, optionally followed by a reason;
blank lines and `#` comments are allowed. A `PAUSED` file blocks all remote
mutations. Preserve current holds when creating the policy: the example is not
a replacement for the live inventory.

```bash
python3 scripts/dev/merge_train_guard.py inspect 1421 \
  --repo-root /home/kilian/dev/vmaf --state-dir /home/kilian/dev/vmaf/.claude/mergetrain
python3 scripts/dev/merge_train_guard.py cycle \
  --repo-root /home/kilian/dev/vmaf --state-dir /home/kilian/dev/vmaf/.claude/mergetrain
```

`inspect` checks one candidate; `cycle` reports the open queue. Both are read-only
without `--apply`. A refused candidate explains its hold, base, ownership, or
missing metadata. Network errors and incomplete inventories never become an
empty, apparently green queue. `cycle` refuses to act if an ineligible PR already
has GitHub auto-merge enabled; its owner must disable that server-side state.

## Ownership and promotion

A checked-out head branch belongs to its existing owner, even if clean. A
detached `agent-*` checkout at the same head is also protected. List additional
detached human checkouts in `protected_worktrees`. The operator must finish its
own work and explicitly hand off ownership; creating fake PID locks or deleting
another actor's checkout is not a handoff. See
[worktree discipline](agent-worktree-discipline.md).

`rebase NUMBER --apply` creates a unique detached checkout under
`STATE/worktrees/pr-NUMBER-*/checkout`, rebases onto fetched master, and pushes
with a lease bound to the observed PR head. Conflicts, rejected pushes, changed
PR metadata, dirty source, and cleanup failures stop that action. The failed
checkout and `owner.json` are retained for the owner to inspect. Only a successful,
clean checkout created by this invocation is removed, using non-force Git.
An existing retained checkout blocks another rebase of that PR until its owner
reviews and hands it off; polling does not create repeated failed worktrees.

`promote NUMBER --apply` performs that same rebase first, reads the resulting
remote head back, then marks the PR ready. Failure never marks it ready or arms
auto-merge. Promotion does not arm a PR; the resulting head needs validation.

## Full local validation and merging

Validation is an explicit owner action. It may run while held or paused, because
it does not mutate GitHub. Supply the exact, clean source checkout and an output
state directory shared with the guarded operator:

```bash
python3 scripts/dev/merge_train_guard.py validate 1421 \
  --repo-root /home/kilian/dev/vmaf --state-dir /home/kilian/dev/vmaf/.claude/mergetrain \
  --checkout /path/to/explicit-validation-checkout --apply
```

The runner executes full `make lint` and `make test`, in that order, and records
both logs. It rejects tracked or untracked source changes before/between/after
the commands, ignored Makefile shadowing, and head changes. Inherited Git
redirection and GNU Make recipe/dry-run overrides are removed. A failed repeat
run archives the older receipt rather than leaving that old pass usable.

Success creates `validated-HEAD.json`, with command/log hashes and an HMAC from
a private local `validation-key`. This is an attestation by the local runner,
not third-party build provenance or proof against an operator who controls the
key and executable toolchain. Handwritten pass booleans, edited receipts, changed
logs, other heads, and receipts from a different gateway version are refused.
Retain the state directory, logs, key permissions, and source/toolchain provenance
together; do not copy an unsigned assertion into a receipt file.

After the source owner has handed off, `merge NUMBER --apply` requires the
exact-head receipt and a nonempty required-check list containing a passing
`Required Checks Aggregator`. Every required check must pass. Pending, skipped,
cancelled, failed, or absent checks block both `merge` and `arm`; GitHub CLI exit
8 (pending) is deliberately a refusal. `arm NUMBER --apply` applies the same
strict gates before requesting GitHub auto-merge. Both use `--match-head-commit`,
never admin bypass or branch deletion.

Prefer `merge` in the automatic loop: future local holds cannot stop auto-merge
already stored on GitHub. When imposing a hold on an armed PR, also disable its
server auto-merge explicitly. GitHub remains responsible for branch protection
after the final local check; local advisory locking cannot serialize unrelated
GitHub writers or arbitrary Git/editor mutations.

`cycle --apply --window 3` performs one bounded pass: merge or rebase eligible
ready PRs, then promote at most one draft when the ready window has room. It uses
ascending PR number within drafts and never launches a repair agent. A rebased
head waits for a fresh full validation receipt. Repair work is an explicit
isolated-owner handoff, outside the automated queue.

## Migrate a running local train

The tracked gateway does not patch, restart, or stop existing services. Before
enabling it, review the exact watchdog/train/operator process identities and
retain their commands, start times, scripts, and hold state as evidence. Stop all
legacy actors that can invoke raw `gh pr ready`, `gh pr merge`, or Git rebases.
Retire an old unrestricted agent process; do not resume its in-memory prompt.

Keep `PAUSED` present while deploying a reviewed, committed gateway to a stable
source checkout. Replace each local runtime entry point with this mapping:

| Legacy entry | Replacement |
| --- | --- |
| `train.sh` | One `cycle --apply` invocation of the reviewed gateway |
| `rebase-clean.sh` | Explicit `rebase NUMBER --apply`; no implicit global sweep |
| `merge_train_operator.py` | Retired mutation operator; use `cycle` for inspection |
| `watchdog.sh` | Supervise only the guarded cycle; never spawn the old agent prompt |

Add the active session's exact branch/worktree owners to `policy.json`, preserve
all holds, and independently confirm that held/non-master PRs have no pending
server auto-merge. Run read-only inspection and the regression command below.
Review the replacement scripts and source hash before removing `PAUSED` and
starting a fresh guarded actor. Replacing a file underneath a running process
does not update that process's policy. A commit landing alone does not complete
this runtime migration.

```bash
python3 -m unittest discover -s scripts/dev/tests -p 'test_merge_train_guard.py'
```

The regressions use disposable Git repositories and Makefiles, with fixture
GitHub responses. They verify control behavior; they do not constitute full
VMAFx native, backend, golden-data, `make lint`, or `make test` acceptance.
