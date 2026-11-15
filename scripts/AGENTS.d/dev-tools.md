---
paths:
  - scripts/dev/*cleanup-agent-state.sh
  - scripts/dev/hw_encoder_corpus.py
  - scripts/dev/project_modernization_audit.py
invariant: Cleanup protects dirty worktrees + strips GIT_*; corpus rename in producer; audit read-only.
---
<!-- markdownlint-disable MD013 MD038 -->
# Developer helpers (`scripts/dev/`)

## Rebase-sensitive invariants

### `dev/cleanup-agent-state.sh` preserves unclassified work

Per [ADR-1239](../../docs/adr/1239-agent-cleanup-preserve-work.md), no arguments
and `--dry-run` = read-only. `--apply` requires exact `--worktree` targets;
keep all dirty, untracked, ignored, active, unknown-owner checkouts protected.
Do not restore forced worktree removal or infer stash redundancy from branch
existence. All stashes and branch refs retained. Temporary-repository
regression = `bash scripts/dev/test-cleanup-agent-state.sh`.

Regression itself must clear inherited `GIT_*`, disable caller
global/system Git configuration before creating fixtures. `git -C` does not
isolate repository, shared metadata, or index paths supplied by environment.
Keep disposable-caller preservation matrix in
`ci/test_git_fixture_isolation.py`, registered in local/CI hooks.

### `dev/hw_encoder_corpus.py` is the canonical corpus producer

Consumed by `tools/ensemble-training-kit/02-generate-corpus.sh`
and LOSO retrain runbook
([ADR-0309](../../docs/adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)).
Producer's output schema (`(src, actual_kbps, vmaf, enc_ms, recipe)`)
currently *not* aligned with
`analyze_knob_sweep.SweepRow` consumer. See
[`ai/AGENTS.md`](../../ai/AGENTS.md) "knob-sweep corpus invariant"
for throw-away wrapper performing rename until
SCHEMA_VERSION=3 lands. **Do not** modify `analyze_knob_sweep.py`
to accept both spellings; producer-side rename = path forward.

### `dev/project_modernization_audit.py` is read-only queue shaping

Modernization audit = operator aid, not CI gate. Scans
curated source/doc roots, model-registry smoke rows, AI script-family
clusters, `.workingdir` state files, emits JSON/Markdown.
Must stay read-only: no automatic edits to `.workingdir/OPEN.md`,
`.workingdir/BACKLOG.md`, `docs/state.md`, GitHub PRs, or changelog
fragments. If future branch wants machine-written backlog updates,
that = separate ADR and module.

Marker scan deliberately suppresses historical closeout wording and
non-debt Python exception plumbing. Live `raise NotImplementedError(...)`
= actionable; docstring saying old `NotImplementedError` scaffold was
replaced, `except NotImplementedError` handler, or custom
`NotImplementedError` subclass is not. Keep that distinction on rebase so
tool does not repopulate `.workingdir` with already-closed gaps.
Same rule applies to documented `-ENOSYS` disabled-build contracts:
workflow comments, API docs, DNN fallback stubs explicitly describing
optional-build behavior are not implementation gaps; bare `return -ENOSYS;`
outside such context still is.
Optional backend contracts naming compile-time guard (`HAVE_*`,
`enable_*=false`), unavailable loader/runtime path, or CPU fallback also
= contract prose, not missing-implementation findings. Test-double prose
("unit tests inject stub") and ADR allocator `.md.stub` reservation wording
likewise suppressed; keep each suppression context-bound so real stubs in
production paths still rank.
Same for non-implementation uses of word "stub": Python type-stub
packages, driver-stub environment diagnostics, comments pinning
disabled-build stub signatures to real implementation ABI.
