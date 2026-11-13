---
paths:
  - scripts/**
invariant: Shell set -euo pipefail, mktemp trap, LC_ALL=C sort; workflow renames in lockstep.
---
<!-- markdownlint-disable MD013 MD038 -->
# Scope, ground rules and governing ADRs (`scripts/`)

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
  gen_ssimulacra2_eotf_lut.py        # sRGB EOTF LUT generator
  run_unittests.sh                   # legacy Python test runner (upstream-mirror)
  test-matrix.sh                     # local docker-matrix harness
```

Catch-all for tooling that does not belong in
`core/tools/` (C CLI lives there) or `tools/` (fork-original
Python/shell user tooling). Most files here fork-original,
no upstream-Netflix equivalent.

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../../AGENTS.md)).
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
- **Wholly-new fork shell scripts carry `Copyright 2026 Lusoris`
  and an SPDX line** per [ADR-0861](../../docs/adr/0861-vmafx-copyright-policy-drop-anthropic.md)
  (supersedes ADR-0025 line format). Upstream-mirror scripts
  (`run_unittests.sh`, parts of `setup/`) keep original headers — no retro-fit.
- **Python helpers under `dev/` import from `ai/`** for shared
  schema helpers (`SweepRow`, knob analysis); keep import side
  free of side-effects (no eager model loads at module import).

## Twin-update awareness

- **Renaming any script** here referenced from
  `.github/workflows/*.yml` requires workflow update in
  **same PR**. Phantom-required gates compound across merge
  train.
- **`scripts/ci/AGENTS.md`** = sister doc for CI tree;
  changes crossing boundary (e.g. moving CI helper out of
  `ci/` into `dev/`) update both AGENTS.md files.

## Governing ADRs

- [ADR-2485](../../docs/adr/2485-vmafx-credits-page.md) —
  credits list, generated page and gate.
- [ADR-0025](../../docs/adr/0025-copyright-handling-dual-notice.md) —
  copyright policy; [ADR-0861](../../docs/adr/0861-vmafx-copyright-policy-drop-anthropic.md)
  = current line format.
- [ADR-0218](../../docs/adr/0218-mobilesal-saliency-extractor.md) —
  MobileSal placeholder.
- [ADR-0221](../../docs/adr/0221-changelog-adr-fragment-pattern.md) —
  fragment-rendered `CHANGELOG.md` and `docs/adr/README.md`.
- [ADR-0309](../../docs/adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md) —
  ensemble retrain runbook.
- [ADR-0937](../../docs/adr/0937-mkdocs-nav-decade-buckets.md) —
  mkdocs ADR nav generator + by-tag indexes.
- [ADR-0752](../../docs/adr/0752-perf-bench-multi-resolution.md) —
  `scripts/perf/bench-multi-resolution.sh` baseline harness.
- [ADR-0907](../../docs/adr/0907-perf-regression-gate-wall-clock.md) —
  `scripts/perf/check-regression.py` wall-clock regression gate
  (CPU-only at first iteration). Gate's tolerance defaults to
  ±5%, baseline lives at `testdata/perf_multi_resolution.json`
  (ADR-0752); intentional perf changes must regenerate baseline
  in same PR. Schema_version drift in baseline file requires
  updating `_index_runs()` in `check-regression.py` in lockstep.
