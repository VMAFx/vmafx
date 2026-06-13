<!-- markdownlint-disable MD038 -->
# AGENTS.md — scripts/

Orientation for agents working on the top-level scripts tree (excluding
`scripts/ci/`, which has its own AGENTS.md). Parent: [../AGENTS.md](../AGENTS.md).

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

This is the catch-all for tooling that doesn't belong in
`core/tools/` (the C CLI lives there) or `tools/` (fork-original
Python/shell user tooling). Most files here are fork-original and
have no upstream-Netflix equivalent.

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md)).
- **`set -euo pipefail` at the top of every shell script.** Pipes
  carry errors; unset variables are fatal. Sourced helpers (file
  starts with `_` and is consumed via `source` / `.`) are the
  exception: they must NOT call `set` at the top level because
  that mutates the caller's shell options — document the exception
  inline. See ADR-0899 (`tools/ensemble-training-kit/_platform_detect.sh`
  is the canonical example).
- **Every `mktemp` call gets a script-wide cleanup trap.**
  Track allocations in a script-scope array and `trap _cleanup
  EXIT INT TERM` so SIGTERM / OOM-kill don't leave orphans in
  `$TMPDIR`. Pattern shown in `scripts/ai/fetch-tiny-blobs.sh`
  and `dev/scripts/smoke-probe-loop.sh` (ADR-0899).
- **`LC_ALL=C` prefixes any `sort` whose output feeds a
  collision check.** Filename-numeric sorts (ADR numbers,
  dispatch-registry symbols) must be locale-stable so the gate
  produces the same answer on the dev box (de_DE.UTF-8), CI
  containers (C.UTF-8), and macOS runners. See ADR-0899 for the
  three scripts where this matters: `scripts/ci/check-adr-numbering.sh`,
  `scripts/ci/check-dispatch-registry.sh`, `scripts/adr/next-free.sh`.
- **All wholly-new fork shell scripts ship the dual Lusoris/Claude
  (Anthropic) copyright header**. Two upstream-mirror scripts
  (`run_unittests.sh`, parts of `setup/`) preserve their original
  headers — do not retro-fit the dual notice on those.
- **Python helpers under `dev/` import from `ai/`** for shared
  schema helpers (`SweepRow`, knob analysis); keep the import side
  free of side-effects (no eager model loads at module import).

## Rebase-sensitive invariants

### `release/concat-changelog-fragments.sh` is the source of truth for `CHANGELOG.md`

Per [ADR-0221](../docs/adr/0221-changelog-adr-fragment-pattern.md),
`CHANGELOG.md` is **rendered** from per-PR fragments under
`changelog.d/<section>/*.md`. The script:

- Renders Keep-a-Changelog section ordering (Added → Changed →
  Deprecated → Removed → Fixed → Security).
- Preserves `changelog.d/_pre_fragment_legacy.md` verbatim at the
  top (migrated content from the pre-fragment Unreleased block).
- Supports `--check` (CI gate) and `--write` (release-please
  rewrite) flags.

**On rebase**: do **not** edit `CHANGELOG.md` by hand. Edit a
fragment, then run the script with `--write`. The CI lane
`docs-fragments` runs `--check` on every PR and fails on drift.
Renaming the script breaks `release-please` config + the CI gate
in the same instant.

**Splice contract (ADR-0913)**: the script anchors the end-of-Unreleased
boundary on `^## \[` — the bracketed form release-please uses for
released sections (`## [vX.Y.Z] - YYYY-MM-DD`) and that
`## [Unreleased]` itself uses. **Do not weaken this regex** to
`^## ` or `^## [^[]` — both shapes were tried previously and both
failed when fragment bodies contained `## ` headers (the 23 k+
line drift PR #332 / #383 / #401 / #384 observed). Fragment
bodies may legitimately contain `## ` or `### ` sub-headings; the
renderer demotes leading `# ` / `## ` to `**bold**` at render time
as defense-in-depth. Authors should still write **bullets, not
headers** per `changelog.d/README.md`; the demoter is for
backwards-compat, not the contract.

**Fragment hygiene**: every fragment lives under one of the six
Keep-a-Changelog section directories (`added/`, `changed/`,
`deprecated/`, `removed/`, `fixed/`, `security/`). Anything else
triggers a stderr WARNING and the fragments inside are skipped.
PR #384 / ADR-0892 catalogued the `perf/` + `performance/`
silent-skip failure mode; ADR-0913 added the visible WARNING.
Empty fragments also emit a stderr WARNING + skip.

### `docs/concat-adr-index.sh` is the source of truth for `docs/adr/README.md`

Per [ADR-0221](../docs/adr/0221-changelog-adr-fragment-pattern.md),
`docs/adr/README.md` is **rendered** from per-ADR fragments under
`docs/adr/_index_fragments/*.md`:

- `_header.md` is the verbatim README prelude (everything before
  `## Index`).
- One Markdown row per ADR, named by the ADR's full slug
  (`NNNN-kebab-case.md`). Slug-keyed for historical reasons: the
  2026-05-02 dedup sweep renumbered duplicate-NNNN ADRs; slug
  filenames remain stable across that remap.
- Rows render oldest-first by ADR ID.

**On rebase**: do **not** edit `docs/adr/README.md` by hand. Add
or edit a fragment file, then run with `--write`. Renaming the
script or changing the slug-keyed naming breaks every ADR-ID
remap downstream.

### `docs/generate-adr-nav.sh` splices a sentinel-bounded block in `mkdocs.yml`

Per [ADR-0937](../docs/adr/0937-mkdocs-nav-decade-buckets.md), the ADR
section of `mkdocs.yml` `nav:` is generated by
`docs/generate-adr-nav.sh` and spliced between the sentinel comments:

```yaml
# >>> ADR-NAV-GENERATED — do not hand-edit; regenerate with
#     scripts/docs/generate-adr-nav.sh --write
...generated block...
# <<< ADR-NAV-GENERATED
```

Editing inside the sentinels by hand will be lost on the next
`--write` run. The script supports `--check` (CI gate) and `--write`
(splice). Per-hundred bucket labels live in the `LABELS` Python dict
inside the script — edit when a bucket's theme drifts.

**On rebase**: keep the sentinel comments in `mkdocs.yml`. If the
sentinels are removed, `--write` exits 66 with a hand-splice
instruction. Renaming the script breaks any future CI `--check` hook;
update `.github/workflows/docs.yml` in the same PR.

### `docs/generate-adr-by-tag.sh` owns the `docs/adr/by-tag/` tree

The script scans every ADR's `Tags:` front-matter field and rewrites
`docs/adr/by-tag/<tag>.md` (one Markdown table per tag) plus
`docs/adr/by-tag/index.md`. Hand-edits inside `by-tag/` are lost on
the next `--write` run. Accepts both `- **Tags**: …` (modern bullet)
and `Tags: …` (legacy bare) forms.

**On rebase**: do not hand-edit `docs/adr/by-tag/*.md`. To add or
remove a tag, edit the ADR's `Tags:` line and re-run the script with
`--write`. Tag values containing whitespace or angle-bracket
placeholders (template examples) are filtered out — keep the filter
in place when adapting the regex.

### `gen_smoke_onnx.py` and `gen_*_onnx.py` are deterministic

The fixture-generation scripts must produce byte-identical output
on re-run. The shipped `model/tiny/smoke_v0.onnx` and
`model/tiny/mobilesal.onnx` are checked-in, sha256-pinned in
`model/tiny/registry.json`, and verified by the C-side loader
(`core/src/dnn/model_loader.c`). A non-deterministic regen breaks
the registry sha256 + the smoke gate. **On rebase**: keep
`onnx.helper.make_model(..., producer_name=..., producer_version=...,
ir_version=...)` pinned at fixed values; do not let `onnx` minor
version drift change the output bytes. The same lesson is encoded
in `ai/AGENTS.md` for the bisect-cache fixtures.

### `gen_ssimulacra2_eotf_lut.py` regen is a Netflix-golden-adjacent event

The generated LUT at `core/src/feature/ssimulacra2_eotf_lut.h`
removes the runtime `libm powf` dependency from the SSIMULACRA 2
hot path. `powf` varies by ~1 ULP between glibc / musl / macOS
libSystem, which compounds to ~2e-4 per-frame drift in the pooled
score. **On rebase**: do not regenerate the LUT casually — a
regen changes the SSIMULACRA 2 fork-added regression-gate values
in `python/test/ssimulacra2_test.py` (per ADR-0164). If the LUT
needs to change, justify it in the commit message and walk the
regression test.

### `setup/detect.sh` is the per-OS dispatcher

The dispatcher reads `/etc/os-release` on Linux and `$OSTYPE` on
macOS to pick `setup/<distro>.sh` or `setup/macos.sh`. Adding a
new distro is one new file (`setup/foo.sh`) plus one branch in
`detect.sh`. The dispatcher is idempotent and never sudo-escalates
without user input — keep both invariants on rebase.

### `dev/hw_encoder_corpus.py` is the canonical corpus producer

Consumed by `tools/ensemble-training-kit/02-generate-corpus.sh`
and by the LOSO retrain runbook
([ADR-0309](../docs/adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)).
The producer's output schema (`(src, actual_kbps, vmaf, enc_ms,
recipe)`) is currently *not* aligned with the
`analyze_knob_sweep.SweepRow` consumer — see
[`ai/AGENTS.md`](../ai/AGENTS.md) "knob-sweep corpus invariant"
for the throw-away wrapper that performs the rename until
SCHEMA_VERSION=3 lands. **Do not** modify `analyze_knob_sweep.py`
to accept both spellings; producer-side rename is the path forward.

### `dev/project_modernization_audit.py` is read-only queue shaping

The modernization audit is an operator aid, not a CI gate. It scans
curated source/doc roots, model-registry smoke rows, AI script-family
clusters, and `.workingdir2` state files, then emits JSON/Markdown.
It must stay read-only: no automatic edits to `.workingdir2/OPEN.md`,
`.workingdir2/BACKLOG.md`, `docs/state.md`, GitHub PRs, or changelog
fragments. If a future branch wants machine-written backlog updates,
that is a separate ADR and module.

The marker scan deliberately suppresses historical closeout wording and
non-debt Python exception plumbing. A live `raise NotImplementedError(...)`
is actionable; a docstring saying that an old `NotImplementedError` scaffold was
replaced, an `except NotImplementedError` handler, or a custom
`NotImplementedError` subclass is not. Keep that distinction on rebase so the
tool does not repopulate `.workingdir2` with already-closed gaps.
The same rule applies to documented `-ENOSYS` disabled-build contracts:
workflow comments, API docs, and DNN fallback stubs that explicitly describe
optional-build behavior are not implementation gaps; bare `return -ENOSYS;`
outside such context still is.
Optional backend contracts that name their compile-time guard (`HAVE_*`,
`enable_*=false`), unavailable loader/runtime path, or CPU fallback are also
contract prose, not missing-implementation findings. Test-double prose
("unit tests inject a stub") and ADR allocator `.md.stub` reservation wording
are likewise suppressed; keep each suppression context-bound so real stubs in
production paths still rank.
Do the same for non-implementation uses of the word "stub": Python type-stub
packages, driver-stub environment diagnostics, and comments that pin
disabled-build stub signatures to the real implementation ABI.

### `git-hooks/` and `githooks/` are two coexisting directories — intentional

Two parallel directories, both fork-original:

- `scripts/git-hooks/` (hyphenated) holds the shared **pre-push**
  PR-body deliverables validator (ADR-0108) plus the pre-rebase
  worktree-drift guard (ADR-0684). These hooks are installed by
  *both* pre-commit paths and are not specific to either.
- `scripts/githooks/` (no hyphen) holds the **native bash pre-commit**
  hook (`pre-commit.sh`) and the unified installer (`install.sh`)
  added in [ADR-0924](../docs/adr/0924-native-pre-commit-hooks.md).
  This directory is the opt-in alternative to the pre-commit
  framework path.

The naming split is deliberate — it lets `make install-hooks`
delegate to `scripts/githooks/install.sh` (a single entry point
that handles both framework and native modes) without colliding
with the existing `scripts/git-hooks/pre-push` symlink target that
the legacy `hooks-install` target wires in.

**On rebase**: the native `pre-commit.sh` mirrors
`.pre-commit-config.yaml`'s file-scope rules (excludes for
`subprojects/`, `core/test/data/`, etc.). When the framework
config changes scope, the native script needs a paired update —
otherwise contributors on the native path drift silently from CI.
The native path is staged-file scope only; CI continues to invoke
`pre-commit run --all-files` against the framework matrix.

### `run_unittests.sh` is upstream-mirror

This script is part of the original Netflix Python test harness
(invokes `python3 -m unittest discover` against `python/test/`).
Keep it byte-identical on rebase. The fork's CI uses meson +
pytest paths; this file ships unchanged for upstream-sync hygiene.

## Twin-update awareness

- **Renaming any script** here that is referenced from
  `.github/workflows/*.yml` requires the workflow update in the
  **same PR**. Phantom-required gates compound across the merge
  train.
- **`scripts/ci/AGENTS.md`** is the sister doc for the CI tree;
  changes that cross the boundary (e.g. moving a CI helper out of
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
  (CPU-only at first iteration). The gate's tolerance defaults to
  ±5% and the baseline lives at `testdata/perf_multi_resolution.json`
  (ADR-0752); intentional perf changes must regenerate the baseline
  in the same PR. Schema_version drift in the baseline file requires
  updating `_index_runs()` in `check-regression.py` in lockstep.
