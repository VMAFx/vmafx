<!-- markdownlint-disable MD060 -->
# ADR-0463: ADM p-norm fast-path split and VIF scalar-fallback malloc hoist

- **Status**: Accepted
- **Date**: 2026-05-16
- **Deciders**: lusoris
- **Tags**: `perf`, `adm`, `vif`, `simd`, `cpu`, `fork-local`

## Context

Profiling (`perf-audit-cpu-2026-05-16.md`, findings 4 and 5) identified two
CPU-side hot-path inefficiencies in the ADM and VIF feature extractors:

**ADM `adm_p_norm == 3.0` inner-loop branch**: `adm_cm_s`,
`adm_csf_den_scale_s`, and `adm_sum_cube_s` each contain `if (adm_p_norm ==
3.0) { x*x*x } else { powf(x, adm_p_norm) }` inside their innermost
accumulation loops. The default value of `adm_p_norm` is `3.0` (set in
`float_adm.c`), so the branch is predictably-taken on every call in normal
operation. The branch prevents auto-vectorisation of the cube accumulation and
wastes branch-predictor state. There are 8+ such branch sites across the three
functions (14 total `if (adm_p_norm == 3.0)` checks inside inner loops).

**VIF `vif_filter1d_*` per-call `aligned_malloc`**: the three scalar fallback
functions `vif_filter1d_s`, `vif_filter1d_sq_s`, and `vif_filter1d_xy_s` each
call `aligned_malloc(ALIGN_CEIL(w * sizeof(float)), MAX_ALIGN)` on every
invocation. On x86 with AVX2, the `ARCH_X86` guard routes to the SIMD path
before the fallback fires, so the malloc is skipped in practice. On ARM64 (no
`ARCH_AARCH64` guard) and non-AVX2 x86, the fallback fires every frame: 3
functions × 4 VIF scales = 12 `aligned_malloc` + `aligned_free` pairs per
frame. The caller (`vif.c:compute_vif`) already preallocates a `tmpbuf` of
size `buf_sz_one = buf_stride * h >= ALIGN_CEIL(w * sizeof(float))` and passes
it as a parameter — the scalar fallback was ignoring it.

## Decision

**Fix 1**: Split each branching function into a p3 fast-path variant
(`adm_cm_s_p3`, `adm_csf_den_scale_s_p3`, `adm_sum_cube_s_p3`) that hardcodes
cube arithmetic and `cbrtf` in place of `powf(x, adm_p_norm)`. The dispatch is
performed once per scale iteration in `compute_adm` with an `if (adm_p_norm ==
3.0)` guard. The generic path is retained for non-default values.

`cbrtf(x) == powf(x, 1.0f/3.0f)` for finite non-negative `x` (IEEE-754
guarantee). Accumulation order is identical in both paths. The change is
bit-exact for all inputs when `adm_p_norm == 3.0`.

**Fix 2**: Replace the per-call `aligned_malloc` / `aligned_free` pattern in
the three scalar VIF filter fallbacks with direct use of the caller-supplied
`tmpbuf` parameter. The caller guarantees `tmpbuf != NULL` and
`sizeof(tmpbuf) >= ALIGN_CEIL(w * sizeof(float))` (ensured by the
`VIF_BUF_CNT`-slab allocation in `compute_vif`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Function cloning via `__attribute__((optimize))` or `-fprofile-generate` PGO | Zero code duplication | Compiler-specific; not guaranteed to specialize; violates JPL-P10 rule (no compiler extensions not in C99/C11 portable subset) | Non-portable, unreliable |
| Macro-based specialization | No duplication of logic | NOLINT-heavy; hard to read; macros banned by coding standard for non-trivial bodies | Violates style guide |
| Move dispatch from `compute_adm` to `adm.h` wrapper inline | Slightly cleaner call site | Inline expansion in header increases build time; wrapper still required | Marginal gain; not worth it |
| Hoist VIF tmpbuf to a separate `VifScalarWorkspace` struct | Clean ownership model | More invasive API change; overkill when `tmpbuf` is already passed | Complexity not justified |

## Consequences

- **Positive**: eliminates 14 per-pixel branch-and-`powf` pairs on the default
  code path in ADM; removes 12 `aligned_malloc`/`aligned_free` pairs per frame
  on ARM64; inner loops in `_p3` variants are auto-vectorisable by GCC/clang.
- **Negative**: `adm_tools.c` is ~500 lines longer (the `_p3` functions). Any
  future change to the accumulation logic in the generic path must be mirrored
  to the `_p3` variants. The `AGENTS.md` invariant documents this.
- **Neutral**: `adm_tools.h` gains 3 new declarations; `adm.c` gains 3 new
  `#define` aliases. No public API change (all new symbols are in `.c` / `.h`
  files internal to `libvmaf`).

## References

- Per-audit findings: `.workingdir/perf-audit-cpu-2026-05-16.md` (findings 4 + 5).
- PR #881 `tmpbuf` hoist precedent: commit `a3123a8dc` introduces the
  `VIF_BUF_CNT`-slab pattern that this ADR extends.
- ADR-0418: double-precision accumulator for ADM sum reductions.
- Source: per user direction (task brief `perf/adm-p-norm-fast-path-vif-arm64-malloc-2026-05-16`).
