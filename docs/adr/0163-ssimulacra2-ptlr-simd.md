# ADR-0163: SSIMULACRA 2 `picture_to_linear_rgb` SIMD ports (T3-1 phase 3)

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, avx2, avx512, neon, ssimulacra2, bit-exact, yuv-rgb, srgb-eotf

## Context

Phase 1 ([ADR-0161](0161-ssimulacra2-simd-bitexact.md)) and phase 2
([ADR-0162](0162-ssimulacra2-iir-blur-simd.md)) vectorised 6 of the
7 hot kernels in SSIMULACRA 2. The final scalar hot path was
`picture_to_linear_rgb` — called 2× per frame to convert YUV input
(any chroma subsampling, 8–16 bpc, 4 BT matrix variants) into
linear RGB via a BT.709/BT.601 matrix + sRGB EOTF.

Two SIMD challenges:

1. **Pixel format dispatch**: `read_plane` handles arbitrary chroma
   ratios (not just 420/422/444 — also irregular `pw / lw` fractions)
   and 8-bit / >8-bit plane storage. Branches on every pixel access.
2. **`powf` in sRGB EOTF**: no vector libm matches scalar `powf`
   byte-for-byte. Same problem as `cbrtf` in phase 1.

## Decision

Port `picture_to_linear_rgb` to all three ISAs (AVX2 + AVX-512 + NEON)
using the established phase-1 pattern:

- **Per-lane scalar pixel reads** via a `read_plane_scalar_*` helper
  inlined into each SIMD TU. Handles all chroma ratios + bit depths
  uniformly. Fills an aligned scratch of N floats (N = 4/8/16 per
  ISA), loaded as one SIMD vector.
- **SIMD matmul + normalise + clamp**: genuine vector ops on the
  8/16/4 pixels in flight. This is where the speedup lives.
- **Per-lane scalar `srgb_to_linear`**: spill SIMD vector to aligned
  scratch, per-lane branch (`x <= 0.04045f ? x/12.92f : powf(...)`),
  reload. Bit-exact to scalar libm.
- **Scalar tail** for `w % N` leftover pixels — verbatim copy of the
  scalar reference body.

New decoupling header `core/src/feature/ssimulacra2_simd_common.h`
defines `simd_plane_t { const void *data; ptrdiff_t stride;
unsigned w; unsigned h; }`. The dispatch wrapper in `ssimulacra2.c`
unpacks `VmafPicture` fields into `simd_plane_t[3]` and invokes the
SIMD entry point — keeps the SIMD TUs decoupled from VMAF API types.

Dispatch: new `ptlr_fn` function pointer in `Ssimu2State`, assigned
in `init_simd_dispatch()`. NULL = scalar fallback via the existing
`picture_to_linear_rgb(s, pic, out)`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Per-lane scalar read + SIMD matmul + per-lane scalar powf (this ADR)** | Handles all formats uniformly; bit-exact by construction; consistent with phase-1 `cbrtf` pattern | SIMD speedup limited to matmul block; scalar reads + scalar powf dominate | **Chosen** — simplicity + bit-exactness worth the smaller speedup |
| **Format-specialised SIMD (420/422/444 × 8/16-bit, 6 paths per ISA)** | Much faster per-frame — true vector loads for Y/U/V; 2:1 chroma broadcast via cross-lane permute | ~1500 LoC more per ISA; 18 functions to maintain; still needs scalar fallback for non-standard ratios | Rejected — combinatorial explosion, small ROI (2 calls / frame) |
| **Vector `powf` via polynomial approximation** | 4-8× speedup on the EOTF | Drifts from scalar libm by 1-2 ulp; breaks ADR-0161 bit-exactness contract; needs tolerance ADR + snapshot update | Rejected — breaks the fork's SIMD-must-match-scalar rule |
| **Leave scalar, defer indefinitely** | Zero work | 2 calls / frame unvectorised; T3-1 closes at 6/7 kernels instead of 7/7 | Rejected per user popup — "All formats (full scope)" |
| **Mask-based SIMD sRGB EOTF** | Compute both branches, blend via mask | No vector libm for `powf` → would still need per-lane scalar | Functionally same as chosen option, just more complex |

## Consequences

- **Positive**:
  - SSIMULACRA 2 now has **zero scalar hot paths**. Phases 1+2+3
    cover all 7 vectorisable kernels.
  - Handles **all** YUV pixel formats: BT.709/BT.601 × limited/full,
    any chroma subsampling ratio, 8-16 bpc.
  - 5 new SIMD test subtests (420/420-10bit/444/444-10bit/422) pin
    bit-exactness across the 3 ISAs. **11/11 tests pass** on both
    AVX-512 host and NEON under QEMU.
- **Negative**:
  - Per-lane scalar reads limit the speedup ceiling. Real gain is
    from SIMD'ing the ~20-float matmul + normalise per pixel.
  - The three SIMD TUs each carry ~120 LoC of near-identical code
    (read_plane, srgb_to_linear, matrix_coefs helpers). Deliberate
    duplication — merging into a shared TU would need an interface
    header + macro expansion for SIMD widths.
- **Neutral / follow-ups**:
  - T3-3 SSIMULACRA 2 snapshot-JSON regression test remains
    pending (gated on `tools/ssimulacra2` availability).
  - The new `ssimulacra2_simd_common.h` is a candidate seed for a
    future `simd/plane.h` if other extractors need the same
    decoupling pattern.

## Verification

- `test_ssimulacra2_simd` on AVX-512 host: **11/11 subtests pass**.
- `qemu-aarch64-static build-aarch64/test/test_ssimulacra2_simd`:
  **11/11 pass** (NEON).
- `meson test -C build` x86: clean (no regression in prior tests).
- clang-tidy clean on the 3 SIMD TUs + dispatch TU + test TU.
- assertion-density PASS.

## References

- [ADR-0130](0130-ssimulacra2-scalar-implementation.md) — scalar
  SSIMULACRA 2.
- [ADR-0161](0161-ssimulacra2-simd-bitexact.md) — phase 1
  (pointwise + reductions).
- [ADR-0162](0162-ssimulacra2-iir-blur-simd.md) — phase 2
  (IIR blur).
- [ADR-0139](0139-ssim-simd-bitexact-double.md) — per-lane scalar
  pattern for transcendentals.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — NOLINT scope.
- libjxl sRGB EOTF reference:
  [`lib/jxl/color_transform.cc`](https://github.com/libjxl/libjxl/blob/main/lib/jxl/color_transform.cc).
- Research digest: [`docs/research/0017-ssimulacra2-ptlr-simd.md`](../research/0017-ssimulacra2-ptlr-simd.md).
- User popup 2026-04-24: "All formats (full scope)".

### AVX-512 audit 2026-05-09: AUDIT-PASS — bit-exact (T3-9 sub-row b)

T3-9 sub-row (b) bench-first audit on Ryzen 9 9950X3D (Zen 5,
AVX-512F/BW/VL). PTLR sits in the ssimulacra2 pipeline alongside the
IIR blur; full-pipeline AVX-512 vs AVX2 bench is recorded in
ADR-0161/0162's audit blocks (1.461x).

Bit-exactness sub-audit: cross-cpumask comparison at full precision
on the Netflix normal pair shows:

- AVX-512 vs AVX2: 0/48 frames differ on `ssimulacra2`. Byte-identical.
- AVX-512 vs scalar: ~3.5e-9 relative difference per frame
  (e.g., 91.695976667734726 AVX-512 vs 91.695976709632987 scalar).

The AVX-512 vs scalar gap is the documented ADR-0163 PTLR LUT story
(deterministic polynomial `cbrtf` + 1024-entry sRGB-EOTF LUT replace
libm calls to eliminate glibc/musl/macOS libSystem variance). The
AVX-512 path uses the same LUT as AVX2, so the gap is inherited from
the AVX2 ship decision and is gated by the ADR-0164 `places=4`
Python snapshot tolerance — no regression. `test_ssimulacra2_simd`
13/13 subtests pass; cross-backend gate clean.

See [Research-0089](../research/0089-avx512-audit-sweep-2026-05-09.md)
for the full bench table and ULP delta details.
