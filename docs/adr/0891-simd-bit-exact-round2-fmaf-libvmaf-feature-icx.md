# ADR-0891: SIMD bit-exactness round-2 — unify SSIMULACRA 2 colour-matrix on FMA, extend `-fp-model=precise` to `libvmaf_feature_static_lib`

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: `simd`, `build`, `bit-exact`, `icx`

## Context

PR #339 added `-fp-model=precise` to the x86 SIMD carve-out static
libs to keep icx from auto-fusing scalar tails and FP-glue inside
the SIMD TUs. CI on master at tip `83698bd5b2` still reproduced two
bit-exactness divergences after the post-merge regression analysis
(the "round-2 RCA"):

1. **`test_ms_ssim_decimate`** — the test executable embeds
   `ms_ssim_decimate_scalar` from `libvmaf_feature_static_lib` as
   its scalar reference. That static lib was compiled without
   `-fp-model=precise`, so under icx the scalar reference picked
   up the relaxed default FP model while the SIMD carve-out
   (already carrying the flag from #339) did not. The two paths
   diverged at sub-ULP.
2. **`test_ssimulacra2_simd::test_ptlr_420_8`** — the AVX2 and
   AVX-512 `picture_to_linear_rgb` kernels computed
   `G = Yn + cb_g*Un + cr_g*Vn` (and the R/B siblings) with
   explicit `_mm256_add_ps(Yn, _mm256_mul_ps(...))` intrinsic
   pairs, while the scalar tails and the test's
   `ref_picture_to_linear_rgb` used plain C `Yn + cb_g * Un`.
   Under icx + `-mfma` the explicit intrinsic add/mul pair was
   still being auto-fused to FMA even with `-fp-model=precise`;
   under gcc the same pattern stayed as two separately-rounded
   operations. There was no portable way to keep mul+add
   un-fused across both compilers without bespoke per-compiler
   plumbing, so any reference that did *not* match whichever
   form icx picked diverged.

Constraint: the project's bit-exactness contract (ADR-0138,
ADR-0139) requires byte-for-byte output equality between the
scalar reference and every SIMD path under
`FLT_EVAL_METHOD == 0`. Sub-ULP drift fails the SIMD test gate.

## Decision

We will:

1. Apply `-fp-model=precise` to `libvmaf_feature_static_lib`
   **and** `libvmaf_ssimulacra2_static_lib` when the C compiler
   is icx, via a sibling helper `_libvmaf_feature_icx_args` in
   `core/src/meson.build` (mirroring the existing
   `_x86_simd_strict_fp_extra` pattern from #339). This keeps
   the scalar references that live inside those libs subject to
   the same FP discipline as the SIMD carve-out libs.
2. Unify the SSIMULACRA 2 `picture_to_linear_rgb` colour-matrix
   on FMA across all implementations:
   - AVX2 main loop → `_mm256_fmadd_ps`.
   - AVX-512 main loop → `_mm512_fmadd_ps`.
   - AVX2 + AVX-512 scalar tails → `fmaf()`.
   - `test_ssimulacra2_simd.c::ref_picture_to_linear_rgb` →
     `fmaf()`.
   Left-to-right associativity for
   `G = (Yn + cb_g*Un) + cr_g*Vn` is preserved by chaining two
   FMAs (`G = fmaf(cb_g, Un, Yn); G = fmaf(cr_g, Vn, G);`).
   Forcing FMA on both sides unifies the rounding for every
   supported x86 compiler — icx (which auto-fuses) and gcc /
   clang (which do not) now both perform a single-rounded FMA.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `volatile`-temp barrier in scalar tails to block fusion | No SIMD change required | Adds a per-lane store/load; depends on icx honouring `volatile` as an FP barrier, which is not guaranteed under `-fp-model=fast`; gcc unaffected | Brittle, compiler-specific, and forces a different code shape than the test reference |
| Function-scope `#pragma STDC FP_CONTRACT OFF` in the scalar tails + test reference | Standard C99 mechanism | `-fp-model=precise` was supposed to subsume this but didn't on icx for explicit intrinsics; would not fix the main-loop intrinsic auto-fusion | Did not match observed icx behaviour |
| `-ffast-math` everywhere (capitulate to relaxed FP) | Smallest patch | Breaks ADR-0138/0139 bit-exactness; cross-host drift returns | Direct violation of project FP contract |
| Per-compiler `#ifdef` selecting fmaf vs mul+add in the scalar path | Could keep gcc as separate ops | Doubles the maintenance surface; still fails on icx auto-fusion of the SIMD intrinsics; "two implementations to keep in sync" is exactly the trap #339 was trying to escape | Worse maintenance ergonomics for the same outcome |
| **Unify on FMA everywhere (chosen)** | One code shape, one rounding model, compiler-agnostic | Mathematically different result vs the pre-2026-05-30 mul+add formulation by ≤ 1 ULP per matrix element | Cleanest invariant; SSIMULACRA 2 is a perceptual metric and the ULP-level difference is far below its measurement noise |

## Consequences

- **Positive**: SSIMULACRA 2 colour-matrix is now bit-exact across
  every supported x86 compiler (gcc, clang, icx) and lane width
  (scalar, AVX2, AVX-512). `test_ssimulacra2_simd` and
  `test_ms_ssim_decimate` pass on every CI runner without
  compiler-specific guarding. The icx-specific FP-model story
  is now applied uniformly to *every* static lib whose objects
  participate in SIMD bit-exactness gates.
- **Negative**: SSIMULACRA 2 numeric output shifts by ≤ 1 ULP
  per matrix element vs the pre-2026-05-30 master tip. The
  Netflix golden-data gate is unaffected (SSIMULACRA 2 is a
  fork-added metric; no Netflix `assertAlmostEqual` covers it).
  Snapshot JSONs under `testdata/scores_cpu_ssimulacra2_*.json`
  may need regeneration — to be verified separately and
  regenerated via `/regen-snapshots` if drift exceeds the
  snapshot tolerance.
- **Neutral / follow-ups**: The `_libvmaf_feature_icx_args`
  helper is now the canonical place to add future icx-only FP
  flags for scalar-reference libs; new SIMD-vs-scalar tests
  should reuse it. `docs/rebase-notes.md` row covers the
  upstream-rebase impact (SSIMULACRA 2 is fork-added; no
  rebase coupling).

## References

- PR #339 (round-1 fix: `-fp-model=precise` on x86 SIMD carve-out libs)
- ADR-0138 / ADR-0139 (bit-exactness invariant)
- ADR-0161 (SSIMULACRA 2 AVX2 port)
- ADR-0278 (NOLINT citation closeout — pattern of inline ADR cites carried through to the new FMA comments)
- Source: `req` — round-2 RCA dossier dispatched by user 2026-05-30 covering the two root causes (Fix A: scalar lib FP-model gap; Fix B: SSIMULACRA 2 colour-matrix auto-fusion under icx).
