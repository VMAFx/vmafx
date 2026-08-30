# ADR-0148: IQA reserved-identifier rename + touched-file lint cascade

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: lint, cleanup, refactor, iqa, touched-file-rule

## Context

The fork inherits the upstream
[IQA library](https://github.com/tjdistler/iqa) under
`core/src/feature/iqa/` plus the IQA-derived `_iqa_*` API
surface used by `ssim.c`, `ms_ssim.c`, `float_ssim.c`,
`float_ms_ssim.c`, and the SIMD bit-exact paths
(`x86/convolve_avx2.c`, `x86/convolve_avx512.c`,
`arm64/convolve_neon.c`). Every `_iqa_*` symbol carries a leading
underscore, which `bugprone-reserved-identifier` /
`cert-dcl37-c` / `cert-dcl51-cpp` flag as reserved-at-file-scope.

The fork's
[rebase-notes 0039 §Follow-up T7-6](../rebase-notes.md) flagged
the rename as a candidate cleanup. On execution, the rename had
two surprises:

1. **No NOLINTs to clear.** A `grep -rn 'NOLINT' core/src/feature/iqa/`
   returned zero hits — the supposed reserved-identifier
   suppressions in `ssim.c` / `ms_ssim.c` / `float_ms_ssim.c`
   never existed; CI simply never inspected those files because
   the diff-based clang-tidy gate only checks files in the
   current PR. The rename's stated motivation was wrong.
2. **Cascading touched-file lint debt.** Per
   [ADR-0141](0141-touched-file-cleanup-rule.md), a PR must leave
   every file it touches lint-clean. The rename touched 21 files;
   each surfaced 3–10 pre-existing warnings invisible to prior
   CI runs (`misc-use-internal-linkage`,
   `readability-isolate-declaration`,
   `readability-function-size`,
   `bugprone-implicit-widening-of-multiplication-result`,
   `clang-analyzer-security.ArrayBound`,
   `clang-analyzer-unix.Malloc`,
   `bugprone-multi-level-implicit-pointer-conversion`,
   `misc-unused-parameters`, …) — about 40 individual warnings
   across 7 files.

After the cascade was visible, the user direction (popup
2026-04-24) was: "Push through with NOLINT brackets for false
positives". This ADR captures the resulting scope.

## Decision

Land a single PR that:

1. **Renames every `_iqa_*` symbol** to its non-reserved form
   across `core/src/feature/`:

   | Was | Now |
   | --- | --- |
   | `_iqa_convolve` | `iqa_convolve` |
   | `_iqa_convolve_set_dispatch` | `iqa_convolve_set_dispatch` |
   | `_iqa_decimate` | `iqa_decimate` |
   | `_iqa_filter_pixel` | `iqa_filter_pixel` |
   | `_iqa_get_pixel` (typedef) | `iqa_get_pixel` |
   | `_iqa_img_filter` | `iqa_img_filter` |
   | `_iqa_ssim` | `iqa_ssim` |
   | `_iqa_ssim_set_dispatch` | `iqa_ssim_set_dispatch` |

2. **Renames the IQA-derived struct/typedef surface** that was
   previously underscore-prefixed:

   | Was | Now |
   | --- | --- |
   | `struct _kernel` | `struct iqa_kernel` |
   | `_ssim_int` (typedef) | `iqa_ssim_int` |
   | `_map_reduce` (typedef) | `iqa_map_reduce` |
   | `_map` (typedef parameter) | `iqa_map_fn` |
   | `_reduce` (typedef parameter) | `iqa_reduce` |
   | `_context` (struct) | `ms_ssim_context` |
   | `_ms_ssim_map` / `_ssim_map` | `ms_ssim_map_fn` / `ssim_map_fn` |
   | `_ms_ssim_reduce` / `_ssim_reduce` | `ms_ssim_reduce_fn` / `ssim_reduce_fn` |
   | `_alloc_buffers` / `_free_buffers` | `ms_ssim_alloc_buffers` / `ms_ssim_free_buffers` |

3. **Renames header guards** to non-reserved spellings:

   | Was | Now |
   | --- | --- |
   | `_CONVOLVE_H_` | `CONVOLVE_INCLUDED` |
   | `_DECIMATE_H_` | `DECIMATE_INCLUDED` |
   | `_SSIM_TOOLS_H_` | `SSIM_TOOLS_INCLUDED` |
   | `__VMAF_MS_SSIM_DECIMATE_H__` | `VMAF_MS_SSIM_DECIMATE_H_INCLUDED` |

4. **Sweeps all touched-file lint warnings** to lint-clean:
   - `misc-use-internal-linkage`: add `static` where the symbol is
     TU-local; otherwise add inline NOLINT with a "Cross-TU:
     declared in X, called from Y" justification (mirrors
     `compute_ssim` in `ssim.c`). Affected: `ssim_map_fn`,
     `ssim_reduce_fn`, `compute_ssim` (cross-TU NOLINT),
     `ms_ssim_map_fn`, `ms_ssim_reduce_fn`,
     `compute_ms_ssim` (cross-TU NOLINT), `ms_ssim_alloc_buffers`,
     `ms_ssim_free_buffers`, `vmaf_fex_float_ssim` /
     `vmaf_fex_float_ms_ssim` (extern-registered NOLINT).
   - `readability-isolate-declaration`: split each multi-decl
     statement into one declaration per line.
   - `readability-function-size`: refactor `calc_ssim`,
     `compute_ssim`, `compute_ms_ssim`, `run_gauss_tests` via
     `static` helpers under the 60-line budget.
   - `bugprone-implicit-widening-of-multiplication-result`: cast
     operands to `size_t` / `ptrdiff_t` at every `malloc(w*h*…)`
     and pointer-offset multiplication site.
   - `bugprone-multi-level-implicit-pointer-conversion`: insert
     explicit `(void *)` cast at the `free(lines)` site.
   - `misc-unused-parameters`: insert `(void)<name>;` at the top
     of each `VmafFeatureExtractor` lifecycle callback (`init`,
     `extract`, `flush`, `close`). These cannot be removed —
     the registry's function-pointer signature is fixed.
   - `clang-analyzer-security.ArrayBound`: scoped
     `NOLINTBEGIN/END` around the inner kernel loops in
     `ssim_accumulate_row` and `ssim_reduce_row_range`. The
     `k_min`/`k_max` clamping mathematically prevents OOB but
     the analyzer can't prove it across the helper boundary.
   - `clang-analyzer-unix.Malloc`: scoped `NOLINTBEGIN/END`
     around the test helpers in `test_iqa_convolve.c` where
     allocations leak by design at process exit.
   - Drop unused `#include "mem.h"` from `ssim.c`; cast unused
     `printf`/`fflush` returns via `(void)`.

5. **Bit-exactness preserved**. `VMAF_CPU_MASK=0` vs `=255` on
   the Netflix golden pair `src01_hrc00/01_576x324` with
   `--feature float_ssim --feature float_ms_ssim` produces
   `diff` exit 0. Full-VMAF `vmaf_v0.6.1` score remains
   `83.856284` on frame 0 (unchanged from master).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Revert and mark T7-6 won't-fix** | No code churn; smallest PR | Leaves the underlying reserved-identifier debt + the eventual touched-file cascade in place; the next contributor who touches `ssim.c` for any reason will hit the same wall | Rejected after popup — the debt is real; clearing it now beats clearing it under feature-PR pressure |
| **Rename only, accept new lint failures (no sweep)** | Smallest semantic-rename diff | Violates ADR-0141 (touched-file rule); CI lint gate would fail | Rejected — the rule is the rule |
| **Rename + revert touches to deep-cascade files (e.g. integer_ssim.c)** | Stops the cascade at file boundaries | Requires keeping the old `_kernel` parameter name in `gaussian_filter_init` so its file stays untouched, which forks the naming inconsistently | Considered after first cascade-round; rejected when user picked "push through" |
| **Per-file disable comments** instead of NOLINT brackets | Zero-impact source | clang-tidy `// NOLINTNEXTLINE` is the project's idiom; per-file disables would require `.clang-tidy` overrides and are harder to grep | Rejected — local NOLINT brackets are the project standard |

## Consequences

- **Positive**:
  - Every reserved-identifier warning across the IQA tree is
    cleared. Future PRs touching `ssim.c` / `ms_ssim.c` /
    `iqa/*.{c,h}` / `convolve_avx2.c` / `convolve_avx512.c`
    no longer trip the touched-file lint cascade — the IQA tree
    is now baseline-clean.
  - `compute_ssim`, `compute_ms_ssim`, `calc_ssim`, and
    `run_gauss_tests` are now decomposed into small named
    `static` helpers — easier to read in stack traces, easier
    to unit-test.
  - The `iqa_*` naming is consistent with `iqa_calc_scale`
    (renamed in T7-5 / ADR-0146) and the helper namespace
    introduced there.
- **Negative**:
  - Upstream-parity cost: future rebases against the original
    IQA library will conflict on every `iqa_kernel` /
    `iqa_ssim_int` / `iqa_map_fn` / `iqa_map_reduce` /
    `iqa_reduce` / `iqa_*` use site. The library is essentially
    abandoned upstream (last meaningful commit pre-2020), so the
    rebase risk is academic. Captured in
    [`docs/rebase-notes.md`](../rebase-notes.md) §0041.
  - The ADR-0141 cascade pattern is now visible: a single rename
    of 21 files surfaced ~40 pre-existing warnings. Future
    cleanup PRs in this area should be expected to do similar
    sweeps.
- **Neutral / follow-ups**:
  - The Netflix upstream IQA library has not been modified for
    this rename. Should upstream IQA ever revive, sync via a
    one-shot mechanical rename of the inverse mapping.
  - One inline NOLINT on `calc_ssim`'s array-bound is a genuine
    static-analyzer false positive — the kernel-offset clamping
    (`k_min = max(0, hkernel_offs - x)`,
    `k_max = min(hkernel_sz, hkernel_sz - (x + hkernel_offs - w + 1))`)
    is provably correct but escapes the analyzer's reach
    across the helper boundary.

## Verification

- `meson test -C build` → 32/32 pass.
- `clang-tidy -p build` over every touched `.c`/`.h` file
  (excluding `arm64/` per the workflow's diff exclusion list) →
  zero warnings.
- Netflix golden pair `src01_hrc00/01_576x324`, full VMAF with
  `vmaf_v0.6.1`: VMAF score, VIF, ADM, MOTION, SSIM all
  bit-identical between `VMAF_CPU_MASK=0` and `=255`.
- Same pair, `--feature float_ssim --feature float_ms_ssim`,
  `VMAF_CPU_MASK=0` vs `=255`: `diff` exit 0.

## References

- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint-clean rule (the policy that drove the cascade).
- [ADR-0146](0146-nolint-sweep-function-size.md) — T7-5
  function-size sweep (introduced the
  `iqa_*` naming convention via `iqa_calc_scale`).
- Backlog item: `.workingdir2/BACKLOG.md` T7-6, originally
  scoped from `docs/rebase-notes.md` §0039 follow-up.
- User direction 2026-04-24 popup: "Land rename + sweep all
  surfaced warnings (full ADR-0141 cleanup)" + later "Push
  through with NOLINT brackets for false positives".
