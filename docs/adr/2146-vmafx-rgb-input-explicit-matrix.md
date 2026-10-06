<!-- markdownlint-disable MD013 MD060 -->
# ADR-2146: RGB, RGBA and BGRA input is converted to Y'CbCr in integers with a matrix, range and transfer the caller states, and refused by name without them

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: maintainer (popup Q-050); RC4 WP13
- **Tags**: api, rc4, gpu, abi, cli, correctness

## Context

The VMAF models and every extractor read Y'CbCr. An RGB frame has no luma until
a matrix says how to make one, and the matrix, the range of the samples and the
transfer decide the score. A guessed matrix gives a plausible and wrong number.
The existing converter (zimg, optional, off by default) takes YUV and gray only.

## Decision

1. **Formats.** `VMAFX_PIXEL_FORMAT_RGB` (R G B), `RGBA` (R G B A) and `BGRA`
   (B G R A), 8 to 16 bits (8 bits in bytes, above in little-endian 16-bit words
   holding the sample in the low bits: FFmpeg `rgb24`, `rgb48le`, `rgba`,
   `rgba64le`, `bgra`, `bgra64le`). The alpha sample is never read. The frame
   made is YUV444P Y'CbCr at the same depth.
2. **A statement is mandatory.** `VmafxFrameImport` gains `rgb_matrix`,
   `rgb_range` (of the R'G'B' samples), `rgb_transfer` and `rgb_out_range` (of
   the Y'CbCr frame; LIMITED is the range the models were trained on). Zero in
   any of them is refused with `VMAFX_E_INVALID` naming the field and the format
   ("none is assumed"). Declared and not converted: BT.2020 constant luminance,
   ICtCp and linear light are refused with `VMAFX_E_NOTSUP` naming them; a value
   that is no enumerator is `VMAFX_E_INVALID`. Other layouts ignore the fields.
3. **Matrices.** BT.601, BT.709 and BT.2020 non-constant luminance, from their
   luma weights (core/api/vmafx.toml `[[color_matrices]]`). The transfer is
   checked and recorded, not applied: the matrix acts on the non-linear code
   values as an encoder does (H.273 equations 20 to 31 with the full-range swing
   2^bpc - 1 and the limited 219 and 224 times 2^(bpc - 8)).
4. **Integer arithmetic, one expression.** Each output sample is a Q30 dot
   product of the pixel's R, G, B with three coefficients plus a constant,
   `(t < 0 ? 0 : t >> 30)` clipped to the code range, in 64-bit integers
   (`rgb_math.h`). The coefficients are rounded once from exact rationals by the
   generator (`rgb_coefficients_gen.h`, per matrix, input range, output range
   and bit depth, because the full swing is not 255 times 2^(bpc - 8)); the
   chroma rows sum to zero, so a gray has exactly mid chroma. The constant
   carries the black levels and the half for rounding (ties round up).
5. **Reference and twins.** The CPU reference is `rgb_convert.h`; the CUDA and
   HIP kernels (`vmafx_import_rgb`) compile the same `rgb_math.h` and take the
   plan by value, so they return the reference's integers (bit-exact, no
   tolerance). The SYCL kernel uses the same header (no fp64, no scratch: the
   expression has no array indexed at run time) and follows in its lane; Metal
   by request.
6. **Oracle.** The model is checked against zimg (FFmpeg `zscale`) for
   full-range input to within the rounding (0.5); zimg treats RGB input as full
   range, so limited-range input rests on the H.273 equations. The C tests
   compare the reference with an exact-rational (`fractions`) oracle on the
   edges of both ranges, the primaries and pseudo-random pixels at 8, 10, 12 and
   16 bits; a pixel whose exact value is within 2^-11 of a tie may differ by
   one.
7. **CLI.** `--pixel_format rgb|rgba|bgra` with `--rgb_matrix`, `--rgb_range`,
   `--rgb_transfer` and `--rgb_out_range`; each missing flag is a usage error
   naming it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Stated matrix, range, transfer, integer reference (chosen) | No guess; twins exact by construction | Callers must state four things | Chosen |
| Default to BT.709 limited | Convenient | Silent wrong scores for BT.601 or full-range sources | Rejected (maintainer) |
| zimg for RGB | Existing library | Off by default, unpinned, not exact across devices | Rejected |
| Floating-point reference | Simple | Twins would need a tolerance | Rejected: integers make the twins exact |
| One shared `range` for both sides | Fewer fields | Cannot score full-range RGB as a limited-range encode | Rejected: `rgb_out_range` |

## Consequences

- **Positive**: RGB sources score with a conversion the caller can name; GPU
  twins equal the CPU.
- **Negative**: ABI 0.1.6 (four appended fields); ICtCp and BT.2020 CL need a
  later decision with their transfer functions.
- **Neutral / follow-ups**: the VmafColor enums of `libvmaf.h` (ADR-2093) lack
  BT.601; mapping the RGB statement to `vmaf_set_input_colorimetry()` follows
  when the stack carries it.

## References

- [ADR-2145](2145-vmafx-input-format-table.md), ADR-2093 (input colorimetry, on master), ADR-1688 / ADR-1679 (named refusals).
- Q-050 (popup, 2026-10-06), answer "Convert with a stated matrix (Recommended)": RGB / RGBA / BGRA, 8 to 16 bits, accepted only with an explicit matrix, range and transfer; one reference conversion plus GPU twins; without a stated matrix the input is refused by name, never guessed.
- Tests: `core/test/test_vmafx_rgb_convert.c`, `scripts/codegen/tests/test_vmafx_api_formats.py`, the RGB cells of `vmafx_format_cells.h`.
