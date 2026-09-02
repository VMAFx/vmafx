<!-- markdownlint-disable MD007 MD041 -->
<!--
  Workflow: SIMD port (AVX-512 widening, NEON sister, AVX2 audit).
  See ADR-0138 / ADR-0139 (bit-exactness invariants) and `/add-simd-path`.
-->
---
name: simd-port
description: Port or audit a SIMD path for an existing feature extractor
agent_type: simd-reviewer
isolation: worktree
worktree_drift_check: true
required_deliverables:

  - digest                             # ADR-0180-style bench-first ceiling decision
  - alternatives                       # AVX2 vs AVX-512 vs NEON vs scalar tail
  - agents_md                          # rebase-sensitive: feature/x86 + arm64 trees
  - reproducer
  - changelog
  - rebase_note
verification:
  netflix_golden: true                 # CPU golden gate is mandatory for SIMD work
  cross_backend_places: 4              # bit-exact vs scalar @ places=4 (ADR-0138/0139)
  meson_test: true
forbidden:
  - modify_netflix_golden_assertions
  - lower_test_thresholds
  - replace_libm_without_lut           # ADR-0164: deterministic LUTs only
master_status_check: true
backlog_id: null                       # set to the relevant T3-x / T7-x row

---

# SIMD port — {{ISA}} {{FEATURE}}

> **MUST RUN BEFORE DISPATCH**:
>
> ```bash
> scripts/ci/agent-eligibility-precheck.py --backlog-id "{{BACKLOG_ID}}"
> ```

## Worktree-isolation prelude

(see `_template.md`)

## Task

Port `{{FEATURE}}` to `{{ISA}}` (one of `avx2`, `avx512`, `neon`).
Use `/add-simd-path {{ISA}} {{FEATURE}}` to scaffold the source +
header + bit-exact-vs-scalar comparison test.

Mandatory steps:

1. **Bench first** if widening AVX2 → AVX-512: re-run on the host
   CPU and compute Amdahl ceiling (DCT vs scalar tail). If the
   projected wall-clock improvement is below 1.3× over AVX2 on the
   Netflix normal pair, **document as a ceiling row** in the
   relevant ADR (ADR-0180 / ADR-0350 pattern) — do **not** ship.
2. Cross-backend diff at `places=4`:

   ```bash
   /cross-backend-diff --feature {{FEATURE}} --places 4
   ```

3. Netflix golden gate:

   ```bash
   make test-netflix-golden
   ```

## Constraints

- ADR-0138 / ADR-0139: per-lane reductions only; no horizontal
  re-association that breaks bit-exactness.
- ADR-0164: replace libm calls with deterministic LUTs to eliminate
  glibc/musl/macOS variance.
- CLAUDE.md §12 r1: golden assertions are immutable.
- `feedback_no_test_weakening`: never lower the `places=4` threshold.

## Deliverables

- Research digest under `docs/research/NNNN-{{ISA}}-{{FEATURE}}-bench.md`
  with ceiling analysis (or `no digest needed: trivial port` only if
  the SIMD path is a pure 1:1 widening with measured ≥1.3× win on
  the golden pair).
- Decision matrix in the ADR's `## Alternatives considered`:
  shipped-as-default vs ceiling-row vs deferred.
- AGENTS.md note in `core/src/feature/x86/AGENTS.md` (or
  `arm64/AGENTS.md` for NEON) listing the new file pair.
- Changelog fragment under `changelog.d/added/` if shipped, or
  `changelog.d/changed/` if widening, or none if ceiling.
- Rebase note: only if the upstream-mirror sibling header changed
  shape — otherwise `no rebase impact: fork-only SIMD twin`.

## Return shape

- Branch name + PR URL.
- Bench numbers: scalar / AVX2 / AVX-512 / NEON @ Netflix normal pair.
- Cross-backend diff worst-ULP at `places=4`.
