#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin integer-ADM CM row rounding before the score's float conversion.

Scale 0 folds through adm_cm_round_row_total_s0(), on unsigned row totals: a
scale-0 row of non-negative cubes can pass INT64_MAX (64-pixel-wide pictures at
the default CSF weights, any width near the ADR-1472 weight budget). Scales 1-3
keep the signed adm_cm_round_row_total() (CUDA's i4 rounding term is negative,
ADR-0155).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

FEATURE_DIR = Path(__file__).resolve().parents[1] / "src" / "feature"
SOURCE_PATHS = {
    "header": "adm_cm_accumulator.h",
    "kernels": "integer_adm_kernels.h",
    "cpu": "integer_adm.c",
    "avx2": "x86/adm_avx2.c",
    "avx512": "x86/adm_avx512.c",
    "cuda": "cuda/integer_adm/adm_cm.cu",
    "hip": "hip/integer_adm/adm_cm.hip",
    "sycl": "sycl/integer_adm_sycl.cpp",
    "metal": "metal/integer_adm.metal",
}

S0_FOLD = "adm_cm_round_row_total_s0("

NATIVE_FOLD_CALLS = (
    (
        "kernels",
        "adm_cm_fold",
        "accum[k] += adm_cm_round_row_total(inner[k], add_shift_inner_accum, shift_inner_accum);",
    ),
    (
        "kernels",
        "adm_cm_fold_s0",
        "accum[k] += adm_cm_round_row_total_s0(inner[k], add_shift_inner_accum, "
        "shift_inner_accum);",
        S0_FOLD,
    ),
    (
        "cuda",
        "i4_cm_flush_row",
        "atomicAdd_int64(band_accum, adm_cm_round_row_total(row_total, "
        "p.add_shift_inner_accum, p.shift_inner_accum));",
    ),
    (
        "cuda",
        "s0_cm_flush_rows",
        "const uint64_cu shifted = adm_cm_round_row_total_s0(row_total, "
        "add_shift_inner_accum, shift_inner_accum);",
        S0_FOLD,
    ),
    (
        "hip",
        "s0_cm_block_reduce",
        "const uint64_cu shifted = adm_cm_round_row_total_s0(row_total, "
        "add_shift_inner_accum, shift_inner_accum);",
        S0_FOLD,
    ),
    (
        "hip",
        "adm_cm_reduce_line_kernel_body",
        "const int64_t shifted = adm_cm_round_row_total(s_row[0], "
        "add_shift_inner_accum, shift_inner_accum);",
    ),
    (
        "sycl",
        "adm_dev_fold_row",
        "auto const shifted = (int64_t)adm_cm_round_row_total_s0(row_total, rounding, shift);",
        S0_FOLD,
    ),
)

METAL_FOLD_CALLS = (
    ("iadm_csf_cm_s0", "cm_out", "total_cm"),
    ("iadm_csf_cm_s123", "cm_out", "total_cm"),
    ("iadm_aim_cm_s0", "out", "total"),
    ("iadm_aim_cm_s123", "out", "total"),
)

# The scalar reference and both x86 twins walk the rows through the shared
# drivers of integer_adm_kernels.h. A driver folds the first row, every
# interior row and the last row: three calls, each after a complete row.
# driver -> its fold: the scale-0 driver folds unsigned rows.
ROW_DRIVERS = {"adm_cm_rows": "adm_cm_fold_s0", "i4_adm_cm_rows": "adm_cm_fold"}
ROW_DRIVER_FOLD = "{fold}(inner, accum, c->add_shift_inner_accum, c->shift_inner_accum);"
ROW_DRIVER_FOLDS = 3
ROW_DRIVER_INTERIOR = "interior_row(c, i, bd, inner);"

# Every reduction entry point hands its rows to a driver exactly once.
ROW_REDUCERS = (
    ("cpu", "adm_cm", "adm_cm_rows(&c, &bd, adm_cm_row, accum);"),
    ("cpu", "i4_adm_cm", "i4_adm_cm_rows(&c, &bd, i4_adm_cm_row, accum);"),
    ("avx2", "adm_cm_avx2", "adm_cm_rows(&c, &bd, cm_row_avx2, accum);"),
    ("avx2", "i4_adm_cm_avx2", "i4_adm_cm_rows(&c, &bd, i4_cm_row_avx2, accum);"),
    ("avx512", "adm_cm_avx512", "adm_cm_rows(&c, &bd, cm_row_avx512, accum);"),
    ("avx512", "i4_adm_cm_avx512", "i4_adm_cm_rows(&c, &bd, i4_cm_row_avx512, accum);"),
)

# An x86 row function only adds to the row accumulator. It never sees the row
# shift, so it cannot round a vector partial or a tail column on its own.
X86_ROLES = ("avx2", "avx512")
X86_FORBIDDEN_TOKENS = ("shift_inner_accum", "adm_cm_round_row_total", "adm_cm_fold")


def _strip_comments(source: str) -> str:
    output: list[str] = []
    index = 0
    quote = ""
    while index < len(source):
        if quote:
            char = source[index]
            output.append(char)
            if char == "\\" and index + 1 < len(source):
                index += 1
                output.append(source[index])
            elif char == quote:
                quote = ""
            index += 1
            continue
        if source[index] in {'"', "'"}:
            quote = source[index]
            output.append(source[index])
            index += 1
            continue
        if source.startswith("//", index):
            newline = source.find("\n", index + 2)
            index = len(source) if newline < 0 else newline
            continue
        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            if end < 0:
                raise ValueError("unterminated C block comment")
            output.append(" ")
            output.extend("\n" for char in source[index : end + 2] if char == "\n")
            index = end + 2
            continue
        output.append(source[index])
        index += 1
    return "".join(output)


def _compact(source: str) -> str:
    return re.sub(r"\s+", "", _strip_comments(source).replace("\\\n", ""))


def _matching_delimiter(source: str, start: int, opening: str, closing: str) -> int:
    depth = 0
    quote = ""
    index = start
    while index < len(source):
        char = source[index]
        if quote:
            if char == "\\":
                index += 2
                continue
            if char == quote:
                quote = ""
        elif char in {'"', "'"}:
            quote = char
        elif char == opening:
            depth += 1
        elif char == closing:
            depth -= 1
            if depth == 0:
                return index
        index += 1
    raise ValueError(f"unterminated {opening}{closing} region")


def _function(source: str, name: str) -> str:
    clean = _strip_comments(source)
    for match in re.finditer(rf"\b{re.escape(name)}\s*\(", clean):
        open_paren = clean.find("(", match.start())
        close_paren = _matching_delimiter(clean, open_paren, "(", ")")
        open_brace = close_paren + 1
        while open_brace < len(clean) and clean[open_brace].isspace():
            open_brace += 1
        if open_brace >= len(clean) or clean[open_brace] != "{":
            continue
        close_brace = _matching_delimiter(clean, open_brace, "{", "}")
        return clean[match.start() : close_brace + 1]
    raise ValueError(f"missing function definition: {name}")


def _sub_exact(source: str, pattern: str, replacement: str) -> str:
    mutated, replacements = re.subn(pattern, replacement, source, count=1)
    if replacements != 1:
        raise AssertionError(f"mutation expected one replacement, got {replacements}: {pattern}")
    return mutated


def _sources() -> dict[str, str]:
    return {
        role: (FEATURE_DIR / relative).read_text(encoding="utf-8")
        for role, relative in SOURCE_PATHS.items()
    }


def _require_exact_call(
    failures: list[str],
    role: str,
    source: str,
    function: str,
    statement: str,
    fold_expression: str = "adm_cm_round_row_total(",
) -> None:
    try:
        body = _compact(_function(source, function))
    except ValueError as error:
        failures.append(f"{role}: {error}")
        return
    call = _compact(statement)
    if body.count(call) != 1:
        failures.append(f"{role}:{function}: missing exact full-row fold")
    if body.count(_compact(fold_expression)) != 1:
        failures.append(f"{role}:{function}: row fold must occur exactly once")


def _require_native_contract(failures: list[str], sources: dict[str, str]) -> None:
    expected_return = "return(row_total+rounding)>>shift;"
    for helper in ("adm_cm_round_row_total", "adm_cm_round_row_total_s0"):
        try:
            body = _compact(_function(sources["header"], helper))
        except ValueError as error:
            failures.append(f"header: {error}")
            continue
        if body.count(expected_return) != 1:
            failures.append("header: raw row fold arithmetic changed")
    s0_helper = re.search(
        r"uint64_t\s+adm_cm_round_row_total_s0\(uint64_t row_total", sources["header"]
    )
    if s0_helper is None:
        failures.append("header: the scale-0 row fold is not unsigned")

    for role, include in {
        "kernels": '#include "adm_cm_accumulator.h"',
        "cpu": '#include "integer_adm_kernels.h"',
        "cuda": '#include "adm_cm_accumulator.h"',
        "hip": '#include "adm_cm_accumulator.h"',
        "sycl": '#include "feature/adm_cm_accumulator.h"',
    }.items():
        if _compact(sources[role]).count(_compact(include)) != 1:
            failures.append(f"{role}: missing private accumulator header")

    for role, function, statement, *fold in NATIVE_FOLD_CALLS:
        _require_exact_call(failures, role, sources[role], function, statement, *fold)


def _require_row_driver_contract(failures: list[str], sources: dict[str, str]) -> None:
    for driver, fold in ROW_DRIVERS.items():
        try:
            body = _compact(_function(sources["kernels"], driver))
        except ValueError as error:
            failures.append(f"kernels: {error}")
            continue
        if body.count(_compact(ROW_DRIVER_FOLD.format(fold=fold))) != ROW_DRIVER_FOLDS:
            failures.append(f"kernels:{driver}: incomplete full-row fold coverage")
        if len(re.findall(r"\badm_cm_fold(?:_s0)?\(", body)) != ROW_DRIVER_FOLDS:
            failures.append(f"kernels:{driver}: a fold escaped the three row folds")
        if body.count(_compact(ROW_DRIVER_INTERIOR)) != 1:
            failures.append(f"kernels:{driver}: interior rows must reach the row callback once")
        if ">>" in body:
            failures.append(f"kernels:{driver}: rounding shift escaped adm_cm_fold")

    for role, function, statement in ROW_REDUCERS:
        try:
            body = _compact(_function(sources[role], function))
        except ValueError as error:
            failures.append(f"{role}: {error}")
            continue
        if body.count(_compact(statement)) != 1:
            failures.append(f"{role}:{function}: rows must go through the shared row driver")


def _require_x86_contract(failures: list[str], sources: dict[str, str]) -> None:
    for role in X86_ROLES:
        body = _compact(sources[role])
        for token in X86_FORBIDDEN_TOKENS:
            if token in body:
                failures.append(f"{role}: {token} escaped the shared row fold")


def _require_metal_contract(failures: list[str], source: str) -> None:
    try:
        metal_helper = _compact(_function(source, "adm_cm_round_row_total"))
    except ValueError as error:
        failures.append(f"metal: {error}")
        return
    if metal_helper.count("return(row_total+rounding)>>shift;") != 1:
        failures.append("metal: raw row fold arithmetic changed")
    metal_statement = (
        "ulong {name} = adm_cm_round_row_total(total, (ulong)c.cm_add_shift_inner, "
        "(uint)c.cm_shift_inner);"
    )
    for function, name, total in METAL_FOLD_CALLS:
        statement = metal_statement.format(name=name).replace("total,", f"{total},")
        _require_exact_call(failures, "metal", source, function, statement)


def _contract_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    _require_native_contract(failures, sources)
    _require_row_driver_contract(failures, sources)
    _require_x86_contract(failures, sources)
    _require_metal_contract(failures, sources["metal"])
    return failures


class AdmCmRowRoundingContractTest(unittest.TestCase):
    def test_live_sources_fold_only_complete_rows(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_per_partition_rounding_mutation_is_detected(self) -> None:
        sources = _sources()
        sources["cuda"] = _sub_exact(
            sources["cuda"],
            r"adm_cm_round_row_total_s0\(row_total,\s*add_shift_inner_accum,",
            "adm_cm_round_row_total_s0(accum_row[row], add_shift_inner_accum,",
        )
        self.assertTrue(
            any("cuda:s0_cm_flush_rows" in item for item in _contract_failures(sources))
        )

    def test_cuda_signed_rounding_mutation_is_detected(self) -> None:
        sources = _sources()
        sources["cuda"] = _sub_exact(
            sources["cuda"],
            r"row_total,\s*p\.add_shift_inner_accum,\s*p\.shift_inner_accum",
            "row_total, (uint32_t)p.add_shift_inner_accum, p.shift_inner_accum",
        )
        self.assertTrue(any("cuda:i4_cm_flush_row" in item for item in _contract_failures(sources)))

    def test_x86_simd_rounding_placement_mutation_is_detected(self) -> None:
        sources = _sources()
        sources["avx2"] = _sub_exact(
            sources["avx2"],
            r"inner\[b\]\s*\+=\s*biased - \(lanes \* f->band\[b\]\.cub_bias\);",
            "inner[b] += (biased - (lanes * f->band[b].cub_bias)) >> c->shift_inner_accum;",
        )
        self.assertTrue(any(item.startswith("avx2:") for item in _contract_failures(sources)))

        sources = _sources()
        sources["avx512"] = _sub_exact(
            sources["avx512"],
            r"inner\[b\]\s*\+=\s*hsum_epu64\(_mm512_add_epi64\(accum_lo\[b\],\s*"
            r"accum_hi\[b\]\)\);",
            "inner[b] += hsum_epu64(_mm512_add_epi64(accum_lo[b], accum_hi[b])) >> "
            "c->shift_inner_accum;",
        )
        self.assertTrue(any(item.startswith("avx512:") for item in _contract_failures(sources)))

    def test_missing_row_fold_mutation_is_detected(self) -> None:
        sources = _sources()
        sources["kernels"] = _sub_exact(
            sources["kernels"],
            r"interior_row\(c, i, bd, inner\);\s*adm_cm_fold_s0\(inner, accum, "
            r"c->add_shift_inner_accum,\s*c->shift_inner_accum\);",
            "interior_row(c, i, bd, inner);",
        )
        self.assertTrue(any("kernels:adm_cm_rows" in item for item in _contract_failures(sources)))

    def test_private_row_loop_mutation_is_detected(self) -> None:
        sources = _sources()
        sources["avx512"] = _sub_exact(
            sources["avx512"],
            r"adm_cm_rows\(&c, &bd, cm_row_avx512, accum\);",
            "cm_row_avx512(&c, bd.start_row, &bd, accum);",
        )
        self.assertTrue(any("avx512:adm_cm_avx512" in item for item in _contract_failures(sources)))

    def test_truncation_mutation_is_detected(self) -> None:
        sources = _sources()
        sources["hip"] = _sub_exact(
            sources["hip"],
            r"adm_cm_round_row_total\(s_row\[0\],\s*add_shift_inner_accum,\s*"
            r"shift_inner_accum\)",
            "s_row[0] >> shift_inner_accum",
        )
        self.assertTrue(any("hip:adm_cm_reduce" in item for item in _contract_failures(sources)))

    def test_post_shift_increment_mutation_is_detected(self) -> None:
        sources = _sources()
        sources["sycl"] = _sub_exact(
            sources["sycl"],
            r"adm_cm_round_row_total_s0\(row_total,\s*rounding,\s*shift\)",
            "adm_cm_round_row_total_s0(row_total, rounding, shift) + 1",
        )
        self.assertTrue(
            any("sycl:adm_dev_fold_row" in item for item in _contract_failures(sources))
        )

    def test_metal_pre_reduction_mutation_is_detected(self) -> None:
        sources = _sources()
        sources["metal"] = _sub_exact(
            sources["metal"],
            r"adm_cm_round_row_total\(total_cm,\s*\(ulong\)c\.cm_add_shift_inner,",
            "adm_cm_round_row_total(local_cm, (ulong)c.cm_add_shift_inner,",
        )
        self.assertTrue(any("metal:iadm_csf_cm_s0" in item for item in _contract_failures(sources)))

    def test_signed_scale0_fold_is_detected(self) -> None:
        # The fold the scale-0 twins had before: the signed helper on an int64
        # row total, which wraps once the row passes INT64_MAX.
        for role, function in (
            ("cuda", "s0_cm_flush_rows"),
            ("hip", "s0_cm_block_reduce"),
            ("sycl", "adm_dev_fold_row"),
        ):
            sources = _sources()
            sources[role] = _sub_exact(
                sources[role],
                r"adm_cm_round_row_total_s0\(row_total,",
                "adm_cm_round_row_total(row_total,",
            )
            with self.subTest(role=role):
                self.assertTrue(
                    any(f"{role}:{function}" in item for item in _contract_failures(sources))
                )
        sources = _sources()
        sources["header"] = _sub_exact(
            sources["header"],
            r"uint64_t\s+adm_cm_round_row_total_s0\(uint64_t row_total",
            "int64_t adm_cm_round_row_total_s0(int64_t row_total",
        )
        self.assertIn("header: the scale-0 row fold is not unsigned", _contract_failures(sources))

    def test_helper_rounding_bias_mutation_is_detected(self) -> None:
        sources = _sources()
        sources["header"] = _sub_exact(
            sources["header"],
            r"row_total\s*\+\s*rounding",
            "row_total",
        )
        self.assertIn("header: raw row fold arithmetic changed", _contract_failures(sources))


if __name__ == "__main__":
    unittest.main()
