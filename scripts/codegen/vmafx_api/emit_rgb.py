# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""core/src/vmafx/rgb_coefficients_gen.h: the Q30 coefficient table of the RGB conversion."""

from __future__ import annotations

from fractions import Fraction

from . import rgb_coefficients
from .emit_formats import NOTICE, SPDX
from .model import Api


def _matrix_values(api: Api) -> dict[str, int]:
    for enum in api.enums:
        if enum.name == "VmafxColorMatrix":
            return {v.name: v.value for v in enum.values}
    return {}


def _block(rows: list[list[int]]) -> str:
    inner = ", ".join("{" + ", ".join(str(c) for c in row) + "}" for row in rows)
    return "{" + inner + "}"


def coefficient_header(api: Api) -> str:
    values = _matrix_values(api)
    by_value = {values[m.enum]: m for m in api.color_matrices}
    size = max(values.values()) + 1
    lines = [
        "/**",
        " *",
        " *  Copyright 2026 Lusoris",
        " *",
        f" * {SPDX}",
        " */",
        "",
        "/*",
        *[
            f" * {line}"
            for line in NOTICE.replace(
                "[[pixel_formats]], ADR-2145", "[[color_matrices]], ADR-2146"
            ).splitlines()
        ],
        " *",
        " * Q30 coefficients of the RGB to Y'CbCr conversion (rgb_convert.h): indexed by the",
        " * VmafxColorMatrix value, the input range and the output range (VmafxColorRange",
        " * value - 1: 0 limited, 1 full), the bits per component - 8, then row Y', Cb', Cr'",
        " * and column R, G, B.",
        " * A matrix without a row is refused before the table is read.",
        " */",
        "",
        "#ifndef VMAFX_RGB_COEFFICIENTS_GEN_H",
        "#define VMAFX_RGB_COEFFICIENTS_GEN_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define VMAFX_RGB_N_MATRICES {size}u",
        f"#define VMAFX_RGB_Q {rgb_coefficients.Q}u",
        f"#define VMAFX_RGB_N_DEPTHS {len(rgb_coefficients.DEPTHS)}u",
        "",
        "/* clang-format off */",
        "static const int64_t vmafx_rgb_coefficients[VMAFX_RGB_N_MATRICES][2][2][VMAFX_RGB_N_DEPTHS][3][3] = {",
    ]
    for index in range(size):
        item = by_value.get(index)
        if item is None:
            lines.append("    {{{{{0}}}}},  /* no conversion: refused by name */")
            continue
        kr, kb = Fraction(item.kr), Fraction(item.kb)
        lines.append(f"    {{  /* {item.enum}: Kr {item.kr}, Kb {item.kb} ({item.standard}) */")
        for rng_in in rgb_coefficients.RANGES:
            lines.append("        {")
            for rng_out in rgb_coefficients.RANGES:
                depths = ",\n".join(
                    "            " + _block(rgb_coefficients.matrix(kr, kb, rng_in, rng_out, bpc))
                    for bpc in rgb_coefficients.DEPTHS
                )
                lines.append(f"          {{ /* {rng_in} -> {rng_out} */\n{depths}}},")
            lines.append("        },")
        lines.append("    },")
    lines += ["};", "/* clang-format on */", "", "#endif /* VMAFX_RGB_COEFFICIENTS_GEN_H */"]
    return "\n".join(lines) + "\n"


def outputs(api: Api) -> list[tuple[str, str]]:
    """(path, text) of the files generated from `[[color_matrices]]`."""
    if not api.color_matrices:
        return []
    return [("core/src/vmafx/rgb_coefficients_gen.h", coefficient_header(api))]
