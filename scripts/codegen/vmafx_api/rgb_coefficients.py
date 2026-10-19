# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Integer coefficients of the RGB to Y'CbCr conversion (ADR-2146).

Exact rational arithmetic from the luma weights of `[[color_matrices]]`, one
rounding to Q30 per coefficient. Used by the emitter of
core/src/vmafx/rgb_coefficients_gen.h and by the tests as the definition the
C reference and the device twins are held to.

For R'G'B' normalised by the input range and Y'CbCr scaled to the output range
(H.273 equations 20 to 31), with Y' = Kr R' + Kg G' + Kb B', Cb' = (B' - Y') /
(2 (1 - Kb)) and Cr' = (R' - Y') / (2 (1 - Kr)), the code values are
    Y  = off_out + (sw_out / sw_in) (Kr R + Kg G + Kb B - off_in)
    Cb = mid + (swc_out / sw_in) (B - Y_in) / (2 (1 - Kb))
    Cr = mid + (swc_out / sw_in) (R - Y_in) / (2 (1 - Kr))
where R, G, B are code values, Y_in = Kr R + Kg G + Kb B and `sw` is the swing of
the range: full 2^bpc - 1 (luma and chroma), limited 219 * 2^(bpc - 8) for R'G'B'
and luma and 224 * 2^(bpc - 8) for chroma. `off` is the black level (full 0,
limited 16 * 2^(bpc - 8)) and `mid` is 2^(bpc - 1). The ratios depend on the bit
depth when a full range is involved (the full swing is not 255 * 2^(bpc - 8)), so
the coefficients are per bit depth. They are those of R, G, B in the three rows;
the constant terms are formed in C from the bit depth.
"""

from __future__ import annotations

from fractions import Fraction

Q = 30
RANGES = ("limited", "full")  # index = VmafxColorRange value - 1
DEPTHS = tuple(range(8, 17))  # bits per component; index = bpc - 8


def swing_luma(rng: str, bpc: int) -> int:
    return 219 * (1 << (bpc - 8)) if rng == "limited" else (1 << bpc) - 1


def swing_chroma(rng: str, bpc: int) -> int:
    return 224 * (1 << (bpc - 8)) if rng == "limited" else (1 << bpc) - 1


def _q(value: Fraction) -> int:
    """Round half to even to Q30."""
    return round(value * (1 << Q))


def matrix(kr: Fraction, kb: Fraction, rng_in: str, rng_out: str, bpc: int) -> list[list[int]]:
    """Rows Y, Cb, Cr; columns R, G, B; the Y row sums to round(a_y * 2^Q), the others to 0."""
    a_y = Fraction(swing_luma(rng_out, bpc), swing_luma(rng_in, bpc))
    a_c = Fraction(swing_chroma(rng_out, bpc), swing_luma(rng_in, bpc))
    y_r, y_b = _q(a_y * kr), _q(a_y * kb)
    y_g = _q(a_y) - y_r - y_b
    cb_r, cb_b = _q(-a_c * kr / (2 * (1 - kb))), _q(a_c / 2)
    cr_r, cr_b = _q(a_c / 2), _q(-a_c * kb / (2 * (1 - kr)))
    return [
        [y_r, y_g, y_b],
        [cb_r, -(cb_r + cb_b), cb_b],
        [cr_r, -(cr_r + cr_b), cr_b],
    ]


def exact(
    kr: Fraction, kb: Fraction, rng_in: str, rng_out: str, bpc: int, rgb: tuple[int, int, int]
) -> tuple[Fraction, Fraction, Fraction]:
    """The unrounded Y', Cb', Cr' code values of one pixel, in exact rationals."""
    s = 1 << (bpc - 8)
    off_in = 16 * s if rng_in == "limited" else 0
    off_out = 16 * s if rng_out == "limited" else 0
    r, g, b = rgb
    kg = 1 - kr - kb
    y_in = kr * r + kg * g + kb * b
    a_y = Fraction(swing_luma(rng_out, bpc), swing_luma(rng_in, bpc))
    a_c = Fraction(swing_chroma(rng_out, bpc), swing_luma(rng_in, bpc))
    mid = 128 * s
    return (
        off_out + a_y * (y_in - off_in),
        mid + a_c * (b - y_in) / (2 * (1 - kb)),
        mid + a_c * (r - y_in) / (2 * (1 - kr)),
    )
