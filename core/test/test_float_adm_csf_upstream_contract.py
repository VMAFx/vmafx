#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin upstream's float arithmetic in the CSF weights of float ADM (ADR-1489).

Two inherited routines compute the weights: ``dwt_quant_step()`` in
``core/src/feature/adm_tools.h`` (Watson) and ``barten_csf()`` with its helpers
in ``core/src/feature/barten_csf_tools.h`` (Barten). Upstream Netflix/vmaf
stores their intermediates in ``float`` and multiplies floats in ``float``.
Fork ports widened those to ``double`` (PR #760, PR #44), which moved every
``float_adm`` score, and ADR-1489 went back to upstream's form.

This test reads the sources:

- the quantisation step keeps ``r``, ``temp`` and ``Q`` in ``float`` and forms
  ``params->k * temp * temp`` without a promoted operand, in the CPU header;
  the Metal twin, which kept its own copy until ADR-1498, takes its weights
  from ``adm_csf_rfactor_s()`` and must not bring a copy back;
- the Barten header promotes no operand of a float product or quotient, and
  writes the promotion of each result out. The SYCL and Metal twins of integer
  ADM compile that header as C++, where ``pow(float, float)`` and
  ``exp(float)`` are the float functions: an implicit promotion would return
  another value there than in C.

Device-free. ``test_float_adm_csf_upstream`` checks the values.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

ADM_TOOLS = "adm_tools.h"
BARTEN = "barten_csf_tools.h"
METAL = "metal/float_adm_metal.mm"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)

# adm_tools.h::dwt_quant_step()
STEP_FLOAT_PRODUCT = re.compile(r"params->k\s*\*\s*temp\s*\*\s*temp")
STEP_FLOAT_LOCALS = ("float r =", "float temp =", "float Q =")
STEP_DOUBLE_LOCAL = re.compile(r"\bdouble\s+(?:r|temp|Q)\b")
STEP_WIDENED_OPERAND = re.compile(r"\(\s*double\s*\)\s*(?:temp\b|params->k\b)")

# metal/float_adm_metal.mm: the CPU's routine, no copy of the step (ADR-1498).
METAL_REFERENCE_CALL = "adm_csf_rfactor_s("
METAL_COPY = re.compile(r"\b\w*dwt_quant_step\s*\(|\b\w*dwt_k_Y\b|\bbarten_csf\s*\(")

# barten_csf_tools.h: the float expressions upstream forms, each with its
# result promoted explicitly where a math function takes it.
BARTEN_REQUIRED = {
    "linear_interpolate": ("((right_value - left_value) / (right_position - left_position))",),
    "barten_rod_cone_sens": ("(double)(cvi_sens_drop / luminance_level)",),
    "barten_mtf": ("exp((double)(-barten_mtf_params_b[i] * spatial_frequency))",),
    "barten_csf": (
        "pow((double)(p_0 * spatial_frequency), (double)p_1)",
        "pow((double)(spatial_frequency / 7), 2)",
        "pow((double)(a * b), 0.5)",
        "(double)(csf * barten_mtf(spatial_frequency) * barten_rod_cone_sens((float)adm_csf_lum_level))",
    ),
}
# An operand promoted before the float operation: the forms PR #44 introduced.
BARTEN_FORBIDDEN = (
    re.compile(r"\(\s*double\s*\)\s*\(\s*right_value\s*-\s*left_value\s*\)"),
    re.compile(r"\(\s*double\s*\)\s*barten_mtf_params_b"),
    re.compile(r"\(\s*double\s*\)\s*p_0\s*\*"),
    re.compile(r"\(\s*double\s*\)\s*spatial_frequency\s*/"),
    re.compile(r"\(\s*double\s*\)\s*a\s*\*\s*b"),
    re.compile(r"\(\s*double\s*\)\s*csf\s*\*"),
)


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE_ROOT / name).read_text(encoding="utf-8")
        for name in (ADM_TOOLS, BARTEN, METAL)
    }


def _function_body(text: str, name: str) -> str | None:
    """Body of the definition of ``name``, comments removed; None when absent."""
    code = COMMENT.sub("", text)
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", code)
    if match is None:
        return None
    depth = 1
    end = match.end()
    while depth > 0 and end < len(code):
        depth += {"{": 1, "}": -1}.get(code[end], 0)
        end += 1
    return " ".join(code[match.end() : end].split())


def _plant(text: str, original: str, planted: str) -> str:
    """``text`` with ``original`` replaced, wherever the formatter broke its lines."""
    pattern = r"\s+".join(re.escape(token) for token in original.split())
    result, count = re.subn(pattern, lambda _match: planted, text)
    if count == 0:
        raise AssertionError(f"`{original}` not found")
    return result


def _step_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    body = _function_body(sources[ADM_TOOLS], "dwt_quant_step")
    if body is None:
        return [f"{ADM_TOOLS}: no definition of dwt_quant_step()"]
    for local in STEP_FLOAT_LOCALS:
        if local not in body:
            failures.append(f"{ADM_TOOLS}: dwt_quant_step() has no `{local}`")
    if STEP_DOUBLE_LOCAL.search(body):
        failures.append(f"{ADM_TOOLS}: dwt_quant_step() keeps an intermediate in double")
    if not STEP_FLOAT_PRODUCT.search(body):
        failures.append(f"{ADM_TOOLS}: dwt_quant_step() does not form params->k * temp * temp")
    if STEP_WIDENED_OPERAND.search(body):
        failures.append(f"{ADM_TOOLS}: dwt_quant_step() promotes an operand of the exponent")
    return failures


def _metal_failures(sources: dict[str, str]) -> list[str]:
    code = COMMENT.sub(" ", sources[METAL])
    failures: list[str] = []
    if METAL_REFERENCE_CALL not in code:
        failures.append(f"{METAL}: the CSF weights do not come from adm_csf_rfactor_s()")
    if METAL_COPY.search(code):
        failures.append(f"{METAL}: a copy of the CSF step is back")
    return failures


def _barten_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for function, required in BARTEN_REQUIRED.items():
        body = _function_body(sources[BARTEN], function)
        if body is None:
            failures.append(f"{BARTEN}: no definition of {function}()")
            continue
        for expression in required:
            if expression not in body:
                failures.append(f"{BARTEN}: {function}() has no `{expression}`")
        for pattern in BARTEN_FORBIDDEN:
            if pattern.search(body):
                failures.append(f"{BARTEN}: {function}() promotes an operand ({pattern.pattern})")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return _step_failures(sources) + _metal_failures(sources) + _barten_failures(sources)


class FloatAdmCsfUpstreamContract(unittest.TestCase):
    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_double_intermediate_of_the_step_is_detected(self) -> None:
        # The form PR #760 carried, one local at a time.
        for local in ("r", "temp", "Q"):
            with self.subTest(local=local):
                sources = _sources()
                planted = sources[ADM_TOOLS].replace(f"float {local} =", f"double {local} =")
                self.assertNotEqual(planted, sources[ADM_TOOLS])
                sources[ADM_TOOLS] = planted
                failures = _contract_failures(sources)
                self.assertTrue(any("keeps an intermediate in double" in item for item in failures))

    def test_widened_exponent_is_detected(self) -> None:
        sources = _sources()
        sources[ADM_TOOLS] = STEP_FLOAT_PRODUCT.sub(
            "params->k * (double)temp * temp", sources[ADM_TOOLS]
        )
        failures = _contract_failures(sources)
        self.assertTrue(any(ADM_TOOLS in item and "promotes" in item for item in failures))

    def test_a_metal_copy_of_the_step_is_detected(self) -> None:
        sources = _sources()
        sources[
            METAL
        ] += "\nstatic float fadm_dwt_quant_step(int lambda, int theta) { return 0; }\n"
        failures = _contract_failures(sources)
        self.assertTrue(any("a copy of the CSF step is back" in item for item in failures))
        sources = _sources()
        sources[METAL] = sources[METAL].replace(METAL_REFERENCE_CALL, "local_rfactor(")
        failures = _contract_failures(sources)
        self.assertTrue(any("adm_csf_rfactor_s()" in item for item in failures))

    def test_every_promoted_barten_operand_is_detected(self) -> None:
        # The forms PR #44 introduced, planted one at a time.
        plants = {
            "((right_value - left_value) /": "((double)(right_value - left_value) /",
            "exp((double)(-barten_mtf_params_b[i] * spatial_frequency))": (
                "exp(-(double)barten_mtf_params_b[i] * spatial_frequency)"
            ),
            "pow((double)(p_0 * spatial_frequency), (double)p_1)": (
                "pow((double)p_0 * spatial_frequency, p_1)"
            ),
            "pow((double)(spatial_frequency / 7), 2)": "pow((double)spatial_frequency / 7, 2)",
            "pow((double)(a * b), 0.5)": "pow((double)a * b, 0.5)",
            "(double)(csf * barten_mtf(spatial_frequency) *": (
                "((double)csf * barten_mtf(spatial_frequency) *"
            ),
        }
        for original, planted in plants.items():
            with self.subTest(planted=planted):
                sources = _sources()
                sources[BARTEN] = _plant(sources[BARTEN], original, planted)
                failures = _contract_failures(sources)
                self.assertTrue(any("promotes an operand" in item for item in failures), failures)

    def test_implicit_promotion_is_detected(self) -> None:
        # Upstream's own text: right in C, another value in a C++ translation unit.
        sources = _sources()
        sources[BARTEN] = _plant(
            sources[BARTEN],
            "pow((double)(p_0 * spatial_frequency), (double)p_1)",
            "pow(p_0 * spatial_frequency, p_1)",
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("barten_csf() has no" in item for item in failures))

    def test_a_cast_in_a_comment_is_not_a_finding(self) -> None:
        sources = _sources()
        sources[BARTEN] = sources[BARTEN].replace(
            "static float barten_mtf(", "/* not `(double)p_0 * f` */\nstatic float barten_mtf("
        )
        self.assertEqual(_contract_failures(sources), [])


if __name__ == "__main__":
    unittest.main()
