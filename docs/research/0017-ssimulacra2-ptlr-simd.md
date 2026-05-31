# Research digest 0017 — SSIMULACRA 2 `picture_to_linear_rgb` SIMD (T3-1 phase 3)

- **Status**: Active (captures decision path for ADR-0163)
- **Related ADRs**: [ADR-0161](../adr/0161-ssimulacra2-simd-bitexact.md)
  (phase 1), [ADR-0162](../adr/0162-ssimulacra2-iir-blur-simd.md)
  (phase 2), [ADR-0163](../adr/0163-ssimulacra2-ptlr-simd.md) (this PR)

## The question

The last scalar hot path in the SSIMULACRA 2 extractor is
`picture_to_linear_rgb` (YUV → linear RGB, 2 calls/frame). It has
two vectorisation obstacles: (1) `read_plane` with arbitrary chroma
subsampling + 8/16-bit dispatch; (2) `powf` in the sRGB EOTF.

How do we SIMD this while preserving the fork's byte-for-byte
bit-exactness contract?

## Key design axes

### Axis 1 — pixel format dispatch

Scalar `read_plane` handles:

- Chroma ratios: `pw == lw` (no subsampling), `pw * 2 == lw` (2:1),
  and arbitrary (via `(int64_t)x * pw / lw`).
- Vertical subsampling: same three cases for `ph` / `lh`.
- Bit depth: 8-bit (uint8) vs >8-bit (uint16).
- Bounds clamping at plane edges.

Options:

1. **Per-lane scalar reads** (chosen): every pixel read goes through
   the scalar helper; fills a float[N] scratch; SIMD loads that.
   Handles all formats uniformly. Limits speedup on reads but keeps
   the matmul SIMD.
2. **Format-specialised SIMD paths**: specialised loaders for 420 /
   422 / 444 × 8 / 16 bit (6 variants per ISA × 3 ISAs = 18
   functions + scalar fallback for arbitrary ratios). Big LoC
   explosion.

Given only 2 calls/frame, option 1 is the right trade-off.

### Axis 2 — sRGB EOTF

Scalar:

```c
if (v <= 0.04045f)
    return v / 12.92f;
return powf((v + 0.055f) / 1.055f, 2.4f);
```

Options:

1. **Per-lane scalar** (chosen): spill SIMD vector to scratch,
   branch per lane, reload. Bit-exact by construction.
2. **Vector polynomial `powf` approximation**: 1–2 ulp drift from
   scalar libm; would need a tolerance ADR + snapshot update.
3. **Mask-based blend**: compute both branches per lane via `powf`;
   still needs per-lane scalar `powf`. Doesn't help.

Option 1 matches the phase-1 `cbrtf` pattern.

### Axis 3 — matmul + normalise SIMD

The `Yn / Un / Vn → R / G / B` matmul is pure SIMD arithmetic on N
lanes. Bit-exact under IEEE-754 lane-commutativity. The normalise
steps (`Y - y_off) * y_scale`) and clamp also SIMD-straightforward.

Scalar matmul uses left-to-right associativity:
`G = Yn + cb_g * Un + cr_g * Vn`. My SIMD implementation preserves
this ordering explicitly: `G = Yn + cb_g*Un; G += cr_g*Vn;`.

### Axis 4 — SIMD TU decoupling

The SIMD TUs must not depend on `VmafPicture` type. Introduced a
new shared header `ssimulacra2_simd_common.h` with a minimal
`simd_plane_t { data, stride, w, h }` struct. The dispatch wrapper
in `ssimulacra2.c` unpacks `VmafPicture` fields into the struct.

## Verification plan

- `test_ssimulacra2_simd` gets 5 new subtests (420-8bit, 420-10bit,
  444-8bit, 444-10bit, 422-8bit) covering the common pixel formats
  two BT matrices.
- Test input: 24×16 pixel frame (wide/height chosen to exercise
  both SIMD main-loop and scalar tail for all 3 ISAs).
- `memcmp` byte-equality of the 3×W×H float output between scalar
  reference and SIMD.

## Outcome

Shipped as `simd/ssimulacra2-picture-to-linear-rgb` branch → PR TBD.
Three SIMD TUs + one new shared header + test expansion + dispatch
update. ~550 LoC per ISA (mostly the per-lane scalar helpers) +
150 LoC test.

## Open questions / follow-ups

- **Format-specialised SIMD paths** — if benchmarking reveals the
  scalar-read bottleneck is limiting overall frame time, consider
  adding 420/444 8bit specialised paths. Low priority.
- **SSIMULACRA 2 snapshot JSON (T3-3)** — still pending.
- **SVE2 port** — shipped under [ADR-0213](../adr/0213-ssimulacra2-sve2.md)
  with qemu validation. The PTLR kernel uses the same per-lane scalar
  read + per-lane scalar `srgb_eotf` pattern documented in this digest;
  the SVE2 mirror is byte-identical to NEON under a fixed 4-lane
  predicate.
- **`picture_to_linear_rgb` is now T3-1 complete** — SSIMULACRA 2
  has no remaining scalar hot paths. Focus shifts to T3-3 snapshot
  gate + other backlog items.
