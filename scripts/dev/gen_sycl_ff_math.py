#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Generate the constants and tables of core/src/feature/ff_math.h.

The header evaluates elementary functions on fp32 pairs (hi + lo). Its
constants are fp32 pairs and triples of pi, ln 2 and reciprocal factorials,
and two tables: atan(j / 16) and sin / cos(k pi / 16). They are computed here
in numpy's extended precision (64-bit significand on x86-64), which is 16 bits
more than a pair holds, and written as hexadecimal fp32 literals between the
header's BEGIN GENERATED / END GENERATED markers.

The header is the fp32 pair math the SYCL, HIP and Metal ciede twins share
(ADR-1436, ADR-1448, ADR-1498); core/src/feature/sycl/sycl_ff_math.h, which
this script is named after, now only names the SYCL primitives it is built on.
Each constant is written through the header's VMAF_FF_CONSTANT and
VMAF_FF_PAIR_INIT, which spell it as a C++17 inline variable with designated
initializers, or, for the Metal subset (VMAF_FF_MSL_SUBSET), in the backend's
program-scope address space with positional ones.

usage: gen_sycl_ff_math.py --write | --check
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

HEADER = Path(__file__).resolve().parents[2] / "core/src/feature/ff_math.h"
BEGIN = "// BEGIN GENERATED\n"
END = "// END GENERATED\n"

LD = np.longdouble
F = np.float32
PI = LD("3.14159265358979323846264338327950288")
LN2 = LD("0.693147180559945309417232121458176568")
# Significand bits of x87 extended precision, without the leading one.
EXTENDED_MANTISSA_BITS = 63
# exp()'s series: 1 / n! is a pair up to this n and an fp32 value beyond.
LAST_PAIR_FACTORIAL = 7
LAST_FACTORIAL = 14


def hexf(value: np.float32) -> str:
    """An fp32 value as a hexadecimal literal."""
    text = float(value).hex()
    if text in ("0x0.0p+0", "-0x0.0p+0"):
        return "0.0f"
    mantissa, exponent = text.split("p")
    return f"{mantissa.rstrip('0').rstrip('.')}p{exponent}f"


def pair(value: np.longdouble) -> tuple[np.float32, np.float32]:
    """value as hi + lo, hi the nearest fp32."""
    high = F(value)
    return high, F(LD(value) - LD(high))


def triple(value: np.longdouble, top_bits: int) -> tuple[np.float32, np.float32, np.float32]:
    """value as a + b + c, a holding `top_bits` significant bits."""
    mantissa, exponent = np.frexp(LD(value))
    first = F(np.ldexp(np.floor(np.ldexp(mantissa, top_bits)), int(exponent) - top_bits))
    rest = LD(value) - LD(first)
    second = F(rest)
    return first, second, F(rest - LD(second))


def pair_line(name: str, value: np.longdouble) -> str:
    high, low = pair(value)
    return f"VMAF_FF_CONSTANT Ff {name} = VMAF_FF_PAIR_INIT({hexf(high)}, {hexf(low)});"


def scalar_lines() -> list[str]:
    lines = [
        "/* NOLINTBEGIN(modernize-use-std-numbers): generated parts of pairs and",
        " * triples. std::numbers has no pairs, and the high word of a pair must be",
        " * the literal its low word was computed against (ADR-1436). */",
        "/* pi / 16 and ln 2 in three parts. The first has 13 significant bits, so",
        " * its product with an integer below 2^11 is exact. */",
    ]
    for name, value in (("kPi16", PI / 16), ("kLn2", LN2)):
        for suffix, part in zip("ABC", triple(value, 13), strict=True):
            lines.append(f"VMAF_FF_CONSTANT float {name}{suffix} = {hexf(part)};")
    lines += [
        f"VMAF_FF_CONSTANT float kSixteenOverPi = {hexf(F(16 / PI))};",
        f"VMAF_FF_CONSTANT float kInvLn2 = {hexf(F(1 / LN2))};",
        pair_line("kPi", PI),
        pair_line("kHalfPi", PI / 2),
        pair_line("kThird", LD(1) / 3),
        pair_line("kFifth", LD(1) / 5),
        pair_line("kSixth", LD(1) / 6),
        pair_line("kOne24", LD(1) / 24),
        pair_line("kOne120", LD(1) / 120),
        "",
        "/* 1 / n! for exp(): pairs where the term needs them, fp32 beyond. */",
    ]
    factorial = LD(2)
    for n in range(3, LAST_FACTORIAL + 1):
        factorial *= n
        if n <= LAST_PAIR_FACTORIAL:
            lines.append(pair_line(f"kInvF{n}", 1 / factorial))
        else:
            lines.append(f"VMAF_FF_CONSTANT float kInvF{n} = {hexf(F(1 / factorial))};")
    return lines


def table_lines() -> list[str]:
    lines = ["", "/* atan(j / 16), j = 0 .. 16, as (hi, lo). */"]
    lines.append("VMAF_FF_CONSTANT float kAtanTable[2 * 17] = {")
    for j in range(17):
        high, low = pair(np.arctan(LD(j) / 16))
        lines.append(f"    {hexf(high)}, {hexf(low)},")
    lines += [
        "};",
        "",
        "/* sin(k pi / 16) and cos(k pi / 16), k = 0 .. 31, as",
        " * (sin hi, sin lo, cos hi, cos lo). */",
        "VMAF_FF_CONSTANT float kSinCosTable[4 * 32] = {",
    ]
    for k in range(32):
        angle = PI * k / 16
        sine, cosine = np.sin(angle), np.cos(angle)
        if k % 8 == 0:  # exact at the multiples of pi / 2
            sine, cosine = LD((0, 1, 0, -1)[k // 8]), LD((1, 0, -1, 0)[k // 8])
        values = [*pair(sine), *pair(cosine)]
        lines.append("    " + ", ".join(hexf(v) for v in values) + ",")
    lines.append("};")
    lines.append("/* NOLINTEND(modernize-use-std-numbers) */")
    return lines


def generated() -> str:
    if np.finfo(LD).nmant < EXTENDED_MANTISSA_BITS:
        raise SystemExit("numpy.longdouble has no extended precision on this host")
    body = ["// clang-format off", *scalar_lines(), *table_lines(), "// clang-format on"]
    return "\n".join(body) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write", action="store_true")
    group.add_argument("--check", action="store_true")
    args = parser.parse_args()

    text = HEADER.read_text(encoding="utf-8")
    start = text.index(BEGIN) + len(BEGIN)
    stop = text.index(END)
    updated = text[:start] + generated() + text[stop:]
    if args.write:
        HEADER.write_text(updated, encoding="utf-8")
        return 0
    if updated != text:
        print(f"{HEADER}: generated block is stale; run {Path(__file__).name} --write")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
