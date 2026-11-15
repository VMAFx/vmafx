---
paths:
  - scripts/git-hooks/*
  - scripts/githooks/**
  - lefthook.yml
invariant: Regular-file hook copies; quote-free lefthook; worktree state sync; merge-base mypy.
---
<!-- markdownlint-disable MD013 MD038 -->
# Git hooks, lefthook bridge and pre-push mypy (`git-hooks/`, `githooks/`)

## Rebase-sensitive invariants

### Git hook dispatch survives installer-worktree deletion

[ADR-1241](../../docs/adr/1241-worktree-hook-dispatch.md) keeps existing
`git-hooks/` source checks and `githooks/` installer/native formatter split.
`githooks/install.py` installs regular copies of `dispatch.sh` into Git's
resolved hooks directory. Never replace them with absolute worktree
symlinks. Unknown hooks refused; recognized replacements get unique
backups. Keep disposable Git lifecycle fixture in
`githooks/tests/test_install.py`, wired into required `Pre-Commit` CI.

Framework `pre-commit`, `commit-msg`, and `pre-push` dispatch must preserve
Git arguments and push-ref stdin through `pre-commit hook-impl`;
pre-rebase source guard installed too. Native mode changes only
pre-commit formatter path. MkDocs and PR-body checks = independent
pre-push config entries: no-PR/draft skip must not skip documentation
validation; selected docs must fail if MkDocs unavailable. Direct
non-doc invocations select scope before requiring docs toolchain.
Paired updates to config, dispatcher, fixture, and
`docs/development/pre-commit-hooks.md` preserve this contract.

### Lefthook run lines stay quote-free; installer yields to lefthook

[ADR-2012](../../docs/adr/2012-lefthook-windows-host.md). Lefthook on
Windows passes each `run:` to `sh -c` unescaped: one line, no double quotes,
logic in `git-hooks/framework-hooks.sh` or sibling script. Bridge prefers
`.venv/bin` / `.venv/Scripts` pre-commit, prepends that dir to `PATH` via
`cygpath` when present (lefthook hands Git Bash `C:/` PWD).
`githooks/install.py` leaves `call_lefthook run` shims in place; order =
`lefthook install`, then `make install-hooks`. `.claude/settings.json` +
`.codex/hooks.json` stay sorted-key JSON (`lefthook uninstall` output).
`githooks/tests/test_install.py` `LefthookBridgeTests` guards all four.

### Post-commit state sync preserves worktree identity

[ADR-1280](../../docs/adr/1280-worktree-state-sync.md) requires
`githooks/state-sync.sh` to synchronize from checkout that created
commit. Linked worktrees use regular-file ledger mirrors; never replace them
with `.workingdir` symlink or run synchronizer against main checkout,
because those paths respectively violate Praetor confinement and record false
Git identity. Keep common-Git lock fail-closed, preserve worktree-local
cache content, and copy only derived `STATE.md` back to canonical private
state. Lock dir carries `owner` (`pid=`, `boot=`); stale = owner ran in other
boot, owner pid gone, or no owner file and older than 2 minutes -> next sync
takes lock over: rename aside, owner recheck, log line. Live owner -> refuse.
Mirror every ledger with its metadata: `bugs.meta.json`,
`questions.meta.json` (Praetor reads one entry per bug / question; missing ->
sync fails in every worktree). real-worktree regression in
`githooks/tests/test_install.py` must cover branch identity, regular files,
metadata files, cache preservation, symlink refusal, and stale versus live
lock owners.

### Python pre-push scope follows the PR merge base

`git-hooks/pre-push-mypy.py` implements parent §12.10 for existing
`ai/` and `scripts/` Python scope. Preserve full merge-base ownership,
including type changes, rather than intersecting with pre-commit's
old-tip/new-tip filenames. Rebases can change imports without changing
owned source file. Keep `always_run: true` and `pass_filenames: false` so
even empty outgoing diff rechecks that set. Resolve symlinks only for
safety validation; keep lexical Git paths for selection and mypy. Reject
outgoing refs different from checked-out HEAD, fail closed on missing
base/tool/file state. Keep `git-hooks/test-pre-push-mypy.py` registered in
local hooks and required Pre-Commit CI. No new lint policy introduced.

Hook fails on findings branch introduces, not inherited ones (ADR-1261).
Two invariants:

- Paths under `ai/src/` = own `mypy` run with `--explicit-package-bases`.
  `ai/src` = `mypy_path` base -> path under it has two module names -> mypy
  refuses run ("Source file found twice under different module names"), so every
  push touching it failed. `exclude` in `pyproject.toml` stops crawl discovery
  only, not path named on command line.
- `scripts/` is explicit package. Keep `scripts/__init__.py`, prepend
  resolved repository root for direct-path execution, and import shared code by
  its canonical `scripts.*` name. real-mypy two-root regression checks
  helper and hook together; implicit namespace gives same file two
  module names and makes every push fail before findings can be reported.
- Same files re-checked at merge base in disposable worktree; only new
  fingerprints fail. Fingerprint = path + error code + message, no line number
  (edit above finding shifts it, does not change it). Worktree removed in
  `finally` (ADR-0332 drift guard). Required `Python Lint` invokes this same
  runner and propagates its status (ADR-1310); do not restore raw directory
  scan or advisory shell tail.
- Blocking runs keep site packages out with `--no-site-packages` and suppress
  only resulting `import-not-found` diagnostics. This makes result
  independent of active environment's PEP 561 packages; resolved local and
  standard-library types remain checked. Hosted CI installs hash-locked
  mypy toolchain and intentionally does not install training stack.

local default base remains `origin/master`. Hosted pull requests use that
same authority; hosted master pushes set `VMAFX_MYPY_BASE_REF` to event's
exact previous commit so post-merge job checks pushed range. Resolve
explicit base through `rev-parse --verify --end-of-options` before merge-base
selection; empty or invalid override must fail closed.

Mypy exit 0 is clean and exit 1 is ordinary findings. Every other status is
blocking analysis error and fails closed with hook exit 2 even when mypy emitted
partial parseable findings; exit 1 with no attributable finding also fails
closed. Do not restore raw exit-status propagation or let earlier exit 1 mask
later blocking status from second module-identity run.

`python_version` is `3.14` and tracks `requires-python` (ADR-1282); do not lower
it. Below 3.12 mypy cannot parse numpy's PEP 695 `type` statement and
`ai/src/` pass aborts before checking anything. delta gate synchronizes
branch's checker configuration files (`pyproject.toml`, `mypy.ini`, `.mypy.ini`,
`setup.cfg`) into disposable baseline worktree while preserving merge-base
source files (T-CI-MYPY-PREPUSH-BASELINE-USES-BASE-CONFIG-2026-09-21). Changes to
checker settings also widen file selection across tracked Python sources in `ai/`
and `scripts/`. Do not evaluate baseline and head under differing checker settings,
and never lower pin.

widened run must resolve `ai/src` through its canonical `vmaf_train.*`
identity exactly once. `pyproject.toml` keeps `follow_imports = "skip"` on
legacy, non-runtime `ai.src.*` compatibility namespace; `ignore_errors` alone is
too late because mypy rejects duplicate module discovery before diagnostics are
filtered. dedicated `ai/src` run remains responsible for checking those
sources with `--explicit-package-bases`.
