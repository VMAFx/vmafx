<!-- markdownlint-disable MD060 -->
# ADR-0245: SIMD bit-exact test harness shared header

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, test, dx, fork-local

## Context

Every fork-added SIMD parity test (`test_psnr_hvs_avx2.c`,
`test_psnr_hvs_neon.c`, `test_moment_simd.c`, `test_motion_v2_simd.c`,
`test_ssimulacra2_simd.c`) re-implements the same scaffolding:

- A `xorshift32` PRNG for reproducible inputs (six file-local copies
  before this change).
- A portable aligned allocator that branches on
  `_MSC_VER || __MINGW32__` — added in PR #198 once and then
  copy-pasted into each new SIMD test.
- An x86 AVX2 CPUID gate (`vmaf_get_cpu_flags_x86()` +
  `VMAF_X86_CPU_FLAG_AVX2` + a `g_has_avx2` static + a "skipping" stderr
  line).
- A `memcmp`-based byte-equal assertion or a relative-tolerance
  assertion, both wrapped in `mu_assert`.

The repeated boilerplate is ~50–100 LOC per file. New SIMD ports
(planned: SSIMULACRA 2 SVE2, additional metric NEON twins) would
multiply the duplication. Each copy is also a potential drift point:
when PR #198 fixed MinGW `aligned_alloc` we had to touch every test
file individually.

The Power-of-10-compliant fix is a single shared header with no behavior
changes — the helpers do not touch the SIMD data path itself; they only
own setup, assertion, and CPU-feature gating.

## Decision

We will introduce `core/test/simd_bitexact_test.h`, a shared `static
inline` header that exposes:

- `simd_test_xorshift32(state)` — Marsaglia 2003 PRNG.
- `simd_test_aligned_malloc(size, align)` /
  `simd_test_aligned_free(ptr)` — POSIX/MinGW/MSVC portable.
- `simd_test_fill_random_f32` / `_u16` / `_i32_mod` — deterministic
  fillers covering the existing tests' input ranges.
- `simd_test_have_avx2()` — runtime CPUID gate for x86, no-op
  elsewhere.
- `SIMD_BITEXACT_ASSERT_MEMCMP(scalar_buf, simd_buf, n_bytes, label)`
  — byte-compares two buffers and prints the first diverging byte
  index on failure.
- `SIMD_BITEXACT_ASSERT_RELATIVE(scalar_val, simd_val, rel_tol, label)`
  — relative-tolerance double comparison (the moment-style
  "tolerance-bounded, not bit-exact" contract per ADR-0179).

The four representative SIMD tests (`test_psnr_hvs_avx2.c`,
`test_psnr_hvs_neon.c`, `test_moment_simd.c`,
`test_motion_v2_simd.c`) migrate to the harness as proof. The
unchanged-ssimulacra2 test stays put for now (its filler has a
near-identical but not byte-identical FP rounding path; migrating it
risks shifting input bit patterns and is out of scope for this PR).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Macros for fixture allocation (e.g. `SIMD_BITEXACT_TEST_FIXTURE(name, w, h, bpc)`) | Cuts the visible test body further; one-line scaffolding | Hides allocation lifetime behind a macro, breaks ASan / UBSan stack traces, fixed structure cannot express `motion_v2`'s 4-buffer adversarial layout, fights Power-of-10 rule 2 (loop bounds visible) | Selected the inline-helper path: explicit `simd_test_aligned_malloc` calls keep lifetime visible and let each test compose its own buffer set. |
| C-template-style helper functions taking a `(scalar_fn, simd_fn)` pair via function pointers | Maximum dedup; one helper drives every test | Forces every kernel to share a signature; the four target tests have wildly different signatures (`compute_1st_moment` vs `od_bin_fdct8x8` vs `motion_score_pipeline_16`); function pointers also defeat clang-tidy's static-call-graph analysis | Rejected: signature heterogeneity dominates. |
| Per-feature harness (one helper header per feature family) | Tightest scoping | Multiplies the maintenance surface — six metric families would mean six near-identical headers | Rejected: the centralised harness is the linker-and-include-tree minimum. |
| Random with fixed seed via `srand`/`rand` | Standard library, no PRNG to maintain | `rand` is in the project's banned-functions list (`docs/principles.md §1.2 rule 30`), and the per-host `RAND_MAX` makes outputs non-portable | Rejected: banned, and reproducibility across CI hosts is a hard requirement for bit-exact tests. |
| Deterministic-pattern (no PRNG; e.g. `pix = i ^ j`) | No PRNG state; trivially auditable | Misses tail-lane and signed-overflow edges that `motion_v2`'s adversarial-negative fixture and `psnr_hvs`'s 12-bit DCT seeds depend on | Rejected as the **default**, kept as one of three named fillers (`simd_test_fill_random_*`). Tests can still hand-author deterministic patterns where they make sense (constant-field, delta-input cases in `test_psnr_hvs_avx2.c`). |

## Consequences

- **Positive**: ~106 LOC removed across the four migrated tests; new
  SIMD parity tests cost ~20 LOC of test-body code instead of ~50–100
  LOC of scaffolding + test body. Future fixes to the
  portable-aligned-allocator (the next MinGW gotcha after PR #198) land
  in one file. The relative-tolerance assertion produces a structured
  failure message (scalar / simd / rel / tol) that the previous
  per-file `mu_assert` text did not.
- **Negative**: Test TUs gain one extra header in their include graph.
  Callers must `#include "test.h"` BEFORE `simd_bitexact_test.h`
  because `test.h` lacks a header guard and would redefine the
  `mu_report` `static inline` if pulled in twice; an inline comment
  in each migrated test calls this out. A future T7-style sweep should
  add header guards to `test.h` and remove the include-order
  constraint.
- **Neutral / follow-ups**: `test_ssimulacra2_simd.c` is intentionally
  untouched in this PR — its `fill_random` formulation differs in FP
  rounding order from `simd_test_fill_random_f32`, and changing the
  input bit pattern of an existing bit-exact test risks masking a
  regression. A separate dedup PR with a SIMD snapshot rerun under
  `/cross-backend-diff` can migrate it. The harness's
  `SIMD_BITEXACT_ASSERT_MEMCMP` should also replace
  `mu_assert("...", memcmp(...) == 0)` patterns elsewhere in the test
  tree opportunistically.

## References

- [PR #198](https://github.com/VMAFx/vmafx/pull/198) — portable
  aligned allocator MinGW fix that motivated centralising the helper.
- [ADR-0138](0138-bit-exact-simd-contract.md) — bit-exactness contract
  for SIMD parity tests.
- [ADR-0140](0140-simd-bit-exact-validation.md) — `places=4` /
  `places=8` validation gates the harness preserves verbatim.
- [ADR-0179](0179-moment-simd-tolerance.md) — moment SIMD tolerance
  contract that drives `SIMD_BITEXACT_ASSERT_RELATIVE`.
- Source: `req` — fork-local DX simplification request 2026-04-29.
