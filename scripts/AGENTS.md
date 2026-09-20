<!-- markdownlint-disable MD038 -->
# AGENTS.md — scripts/

Orientation for agents on top-level scripts tree (excluding
`scripts/ci/`, own AGENTS.md). Parent: [../AGENTS.md](../AGENTS.md).

## Scope

```text
scripts/
  ci/               # CI utilities — separate AGENTS.md (see scripts/ci/AGENTS.md)
  dev/              # developer-time helpers (corpus generation, knob analysis)
  docs/             # ADR-0221 ADR-index fragment concatenation
  git-hooks/        # framework pre-push hook (PR-body validator; ADR-0108)
  githooks/         # native bash pre-commit hook + installer (opt-in; ADR-0924)
  release/          # ADR-0221 CHANGELOG.md fragment concatenation
  setup/            # OS/distro setup dispatcher + per-distro scripts
  gen_smoke_onnx.py                  # tiny-AI smoke fixture generator (deterministic)
  gen_mobilesal_placeholder_onnx.py  # MobileSal placeholder fixture (T6-2a, ADR-0218)
  gen_ssimulacra2_eotf_lut.py        # sRGB EOTF LUT generator
  run_unittests.sh                   # legacy Python test runner (upstream-mirror)
  test-matrix.sh                     # local docker-matrix harness
```

Catch-all for tooling that does not belong in
`core/tools/` (C CLI lives there) or `tools/` (fork-original
Python/shell user tooling). Most files here fork-original,
no upstream-Netflix equivalent.

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md)).
- **`set -euo pipefail` at top of every shell script.** Pipes
  carry errors; unset variables fatal. Sourced helpers (file
  starts with `_`, consumed via `source` / `.`) = exception:
  must NOT call `set` at top level — mutates caller's shell options —
  document exception inline. See ADR-0899
  (`tools/ensemble-training-kit/_platform_detect.sh` = canonical
  example).
- **Every `mktemp` call gets script-wide cleanup trap.**
  Track allocations in script-scope array, `trap _cleanup EXIT INT TERM`
  so SIGTERM / OOM-kill don't leave orphans in
  `$TMPDIR`. Pattern shown in `scripts/ai/fetch-tiny-blobs.sh`
  and `dev/scripts/smoke-probe-loop.sh` (ADR-0899).
- **`LC_ALL=C` prefixes any `sort` feeding collision check.**
  Filename-numeric sorts (ADR numbers, dispatch-registry symbols)
  must be locale-stable. Gate must produce same answer on dev box
  (de_DE.UTF-8), CI containers (C.UTF-8), macOS runners. Three
  scripts affected: `scripts/ci/check-adr-numbering.sh`,
  `scripts/ci/check-dispatch-registry.sh`, `scripts/adr/next-free.sh`.
  See ADR-0899.
- **All wholly-new fork shell scripts ship dual Lusoris/Claude
  (Anthropic) copyright header**. Two upstream-mirror scripts
  (`run_unittests.sh`, parts of `setup/`) preserve original
  headers — do not retro-fit dual notice on those.
- **Python helpers under `dev/` import from `ai/`** for shared
  schema helpers (`SweepRow`, knob analysis); keep import side
  free of side-effects (no eager model loads at module import).

## Rebase-sensitive invariants

### `sync-pelorus-interop.sh` reads one exact source object (ADR-1113)

The Pelorus mirror guard pins a full 40-character released commit and reads
every source with `git show`. A non-Git source directory or a checkout missing
that object fails closed; never restore fallback to working-tree bytes. Re-pin
for reviewed ABI additions and released parser correctness/security fixes,
even when ABI 1.3 is unchanged. The conformance fixture body is byte-sensitive
through EOF apart from the documented include rewrite; lint/format exclusions
belong in VMAFx tooling, not inside that shared body. Keep the guard, its
hermetic test, and the required Pre-Commit workflow checkout in one change.

### `dev/cleanup-agent-state.sh` preserves unclassified work

Per [ADR-1239](../docs/adr/1239-agent-cleanup-preserve-work.md), no arguments
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

### `release/concat-changelog-fragments.sh` is the source of truth for `CHANGELOG.md`

Per [ADR-0221](../docs/adr/0221-changelog-adr-fragment-pattern.md),
`CHANGELOG.md` **rendered** from per-PR fragments under
`changelog.d/<section>/*.md`. Script:

- Renders Keep-a-Changelog section ordering (Added → Changed →
  Deprecated → Removed → Fixed → Security).
- Preserves `changelog.d/_pre_fragment_legacy.md` verbatim at top
  (migrated content from pre-fragment Unreleased block).
- Supports `--check` (CI gate) and `--write` (`release-please`
  rewrite) flags.

**On rebase**: do **not** edit `CHANGELOG.md` by hand. Edit
fragment, run script with `--write`. CI lane
`docs-fragments` runs `--check` on every PR, fails on drift.
Renaming script breaks `release-please` config + CI gate
in same instant.

**Splice contract (ADR-0913)**: script anchors end-of-Unreleased
boundary on `^## \[` — bracketed form `release-please` uses for
released sections (`## [vX.Y.Z] - YYYY-MM-DD`) and
`## [Unreleased]` itself uses. **Do not weaken this regex** to
`^## ` or `^## [^[]` — both shapes tried previously, both
failed when fragment bodies contained `## ` headers (23 k+
line drift PR #332 / #383 / #401 / #384 observed). Fragment
bodies may legitimately contain `## ` or `### ` sub-headings;
renderer demotes leading `# ` / `## ` to `**bold**` at render
time as defense-in-depth. Authors should still write **bullets,
not headers** per `changelog.d/README.md`; demoter for
backwards-compat, not the contract.

**Fragment hygiene**: every fragment lives under one of six
Keep-a-Changelog section directories (`added/`, `changed/`,
`deprecated/`, `removed/`, `fixed/`, `security/`). Anything else
triggers stderr WARNING, fragments inside skipped.
PR #384 / ADR-0892 catalogued `perf/` + `performance/`
silent-skip failure mode; ADR-0913 added visible WARNING.
Empty fragments also emit stderr WARNING + skip.

### `docs/concat-adr-index.sh` is the source of truth for `docs/adr/README.md`

Per [ADR-0221](../docs/adr/0221-changelog-adr-fragment-pattern.md),
`docs/adr/README.md` **rendered** from per-ADR fragments under
`docs/adr/_index_fragments/*.md`:

- `_header.md` = verbatim README prelude (everything before
  `## Index`).
- One Markdown row per ADR, named by ADR's full slug
  (`NNNN-kebab-case.md`). Slug-keyed for historical reasons:
  2026-05-02 dedup sweep renumbered duplicate-NNNN ADRs; slug
  filenames remain stable across that remap.
- Rows render oldest-first by ADR ID.

**On rebase**: do **not** edit `docs/adr/README.md` by hand. Add
or edit fragment file, run with `--write`. Renaming script
or changing slug-keyed naming breaks every ADR-ID remap
downstream.

### `docs/generate-adr-nav.sh` splices a sentinel-bounded block in `mkdocs.yml`

Per [ADR-0937](../docs/adr/0937-mkdocs-nav-decade-buckets.md), ADR
section of `mkdocs.yml` `nav:` generated by
`docs/generate-adr-nav.sh`, spliced between sentinel comments:

```yaml
# >>> ADR-NAV-GENERATED — do not hand-edit; regenerate with
#     scripts/docs/generate-adr-nav.sh --write
...generated block...
# <<< ADR-NAV-GENERATED
```

Editing inside sentinels by hand lost on next
`--write` run. Script supports `--check` (CI gate) and `--write`
(splice). Per-hundred bucket labels live in `LABELS` Python dict
inside script — edit when bucket's theme drifts.

**On rebase**: keep exactly one ordered sentinel pair in `mkdocs.yml`.
Both write and check fail before changing output if either boundary
missing, repeated, or reversed. Generate tags before navigation with
`make docs-fragments-write`. Required Docs CI and local pre-commit run
`make docs-fragments-check`; preserve that shared entry point and
fixture in `docs/tests/test_generators.py` (ADR-1242).

### `docs/generate-adr-by-tag.sh` owns the `docs/adr/by-tag/` tree

Script scans every ADR's `Tags:` front-matter field, rewrites
`docs/adr/by-tag/<tag>.md` (one Markdown table per tag) plus
`docs/adr/by-tag/index.md`. Hand-edits inside `by-tag/` lost on
next `--write` run. Accepts both `- **Tags**: …` (modern bullet)
and `Tags: …` (legacy bare) forms.

**On rebase**: do not hand-edit `docs/adr/by-tag/*.md`. To add or
remove tag, edit ADR's `Tags:` line, re-run script with
`--write`. Tag values containing whitespace or angle-bracket
placeholders (template examples) filtered out — keep filter
in place when adapting regex. Validate output basenames before writing,
deduplicate equivalent tags within one ADR, preserve generator-owned
lint header. Title escaping and meaningful code-span spaces = renderer
responsibilities; do not rewrite accepted ADRs to satisfy generated lint.
`docs/check-adr-index.py` also verifies fragment/source coverage and mutable
fragment ADR references before concatenation.

### `gen_smoke_onnx.py` and `gen_*_onnx.py` are deterministic

Fixture-generation scripts must produce byte-identical output
on re-run. Shipped `model/tiny/smoke_v0.onnx` and
`model/tiny/mobilesal.onnx` checked-in, sha256-pinned in
`model/tiny/registry.json`, verified by C-side loader
(`core/src/dnn/model_loader.c`). Non-deterministic regen breaks
registry sha256 + smoke gate. **On rebase**: keep
`onnx.helper.make_model(..., producer_name=..., producer_version=...,
ir_version=...)` pinned at fixed values; do not let `onnx` minor
version drift change output bytes. Same lesson encoded
in `ai/AGENTS.md` for bisect-cache fixtures.

### `gen_ssimulacra2_eotf_lut.py` regen is a Netflix-golden-adjacent event

Generated LUT at `core/src/feature/ssimulacra2_eotf_lut.h`
removes runtime `libm powf` dependency from SSIMULACRA 2
hot path. `powf` varies ~1 ULP between glibc / musl / macOS
libSystem, compounding to ~2e-4 per-frame drift in pooled
score. **On rebase**: do not regenerate LUT casually — regen
changes SSIMULACRA 2 fork-added regression-gate values
in `python/test/ssimulacra2_test.py` (per ADR-0164). If LUT
needs change, justify in commit message, walk regression test.

### `setup/detect.sh` is the per-OS dispatcher

Dispatcher reads `/etc/os-release` on Linux, `$OSTYPE` on
macOS, picks `setup/<distro>.sh` or `setup/macos.sh`. Adding
new distro = one new file (`setup/foo.sh`) plus one branch in
`detect.sh`. Dispatcher idempotent, never sudo-escalates
without user input — keep both invariants on rebase.

### `dev/hw_encoder_corpus.py` is the canonical corpus producer

Consumed by `tools/ensemble-training-kit/02-generate-corpus.sh`
and LOSO retrain runbook
([ADR-0309](../docs/adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)).
Producer's output schema (`(src, actual_kbps, vmaf, enc_ms, recipe)`)
currently *not* aligned with
`analyze_knob_sweep.SweepRow` consumer. See
[`ai/AGENTS.md`](../ai/AGENTS.md) "knob-sweep corpus invariant"
for throw-away wrapper performing rename until
SCHEMA_VERSION=3 lands. **Do not** modify `analyze_knob_sweep.py`
to accept both spellings; producer-side rename = path forward.

### `dev/project_modernization_audit.py` is read-only queue shaping

Modernization audit = operator aid, not CI gate. Scans
curated source/doc roots, model-registry smoke rows, AI script-family
clusters, `.workingdir2` state files, emits JSON/Markdown.
Must stay read-only: no automatic edits to `.workingdir2/OPEN.md`,
`.workingdir2/BACKLOG.md`, `docs/state.md`, GitHub PRs, or changelog
fragments. If future branch wants machine-written backlog updates,
that = separate ADR and module.

Marker scan deliberately suppresses historical closeout wording and
non-debt Python exception plumbing. Live `raise NotImplementedError(...)`
= actionable; docstring saying old `NotImplementedError` scaffold was
replaced, `except NotImplementedError` handler, or custom
`NotImplementedError` subclass is not. Keep that distinction on rebase so
tool does not repopulate `.workingdir2` with already-closed gaps.
Same rule applies to documented `-ENOSYS` disabled-build contracts:
workflow comments, API docs, DNN fallback stubs explicitly describing
optional-build behavior are not implementation gaps; bare `return -ENOSYS;`
outside such context still is.
Optional backend contracts naming compile-time guard (`HAVE_*`,
`enable_*=false`), unavailable loader/runtime path, or CPU fallback also
= contract prose, not missing-implementation findings. Test-double prose
("unit tests inject a stub") and ADR allocator `.md.stub` reservation wording
likewise suppressed; keep each suppression context-bound so real stubs in
production paths still rank.
Same for non-implementation uses of word "stub": Python type-stub
packages, driver-stub environment diagnostics, comments pinning
disabled-build stub signatures to real implementation ABI.

### Git hook dispatch survives installer-worktree deletion

[ADR-1241](../docs/adr/1241-worktree-hook-dispatch.md) keeps existing
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
  only, not a path named on command line.
- Same files re-checked at merge base in disposable worktree; only new
  fingerprints fail. Fingerprint = path + error code + message, no line number
  (edit above a finding shifts it, does not change it). Worktree removed in
  `finally` (ADR-0332 drift guard). CI `mypy` = advisory
  (`|| echo` in `Python Lint`), inherited findings vary with installed
  numpy / pandas / torch stubs.

Non-zero exit with no attributable finding = mypy broke -> fail closed, exit 2.
Do not restore raw exit-status propagation. Do not raise `python_version` from
`3.10` here: stale, but `3.14` unmasks 175 findings on master
(T-CI-MYPY-PYTHON-VERSION-STALE-2026-09-19).

### `run_unittests.sh` is upstream-mirror

Script = part of original Netflix Python test harness
(invokes `python3 -m unittest discover` against `python/test/`).
Keep byte-identical on rebase. Fork's CI uses meson +
pytest paths; file ships unchanged for upstream-sync hygiene.

## Twin-update awareness

- **Renaming any script** here referenced from
  `.github/workflows/*.yml` requires workflow update in
  **same PR**. Phantom-required gates compound across merge
  train.
- **`scripts/ci/AGENTS.md`** = sister doc for CI tree;
  changes crossing boundary (e.g. moving CI helper out of
  `ci/` into `dev/`) update both AGENTS.md files.

## Governing ADRs

- [ADR-0025](../docs/adr/0025-copyright-handling-dual-notice.md) —
  dual-copyright policy.
- [ADR-0218](../docs/adr/0218-mobilesal-saliency-extractor.md) —
  MobileSal placeholder.
- [ADR-0221](../docs/adr/0221-changelog-adr-fragment-pattern.md) —
  fragment-rendered `CHANGELOG.md` and `docs/adr/README.md`.
- [ADR-0309](../docs/adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md) —
  ensemble retrain runbook.
- [ADR-0937](../docs/adr/0937-mkdocs-nav-decade-buckets.md) —
  mkdocs ADR nav generator + by-tag indexes.
- [ADR-0752](../docs/adr/0752-perf-bench-multi-resolution.md) —
  `scripts/perf/bench-multi-resolution.sh` baseline harness.
- [ADR-0907](../docs/adr/0907-perf-regression-gate-wall-clock.md) —
  `scripts/perf/check-regression.py` wall-clock regression gate
  (CPU-only at first iteration). Gate's tolerance defaults to
  ±5%, baseline lives at `testdata/perf_multi_resolution.json`
  (ADR-0752); intentional perf changes must regenerate baseline
  in same PR. Schema_version drift in baseline file requires
  updating `_index_runs()` in `check-regression.py` in lockstep.
