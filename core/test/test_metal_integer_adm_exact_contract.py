#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin integer_adm_metal's decouple to the CPU's integer arithmetic (ADR-1498, ADR-1413).

The CPU's decouple (``adm_decouple_band()`` / ``adm_decouple_band_s123()`` in
``integer_adm_kernels.h``) takes the reciprocal 2^30 / o from ``div_lookup``,
an integer division, and stores ``MIN(rst * gain, t)`` / ``MAX(rst * gain, t)``,
the double product of the restored sample and ``adm_enhn_gain_limit``
truncated toward zero. ``integer_adm_metal`` took the reciprocal in fp32 at
scale 0 and multiplied by the limit in binary32
(T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01).

The kernel now calls ``vmaf_mtl_iadm_decouple_s0()`` / ``_s123()`` of
``metal_integer_adm_math.h``: the integer reciprocal, and
``adm_gain_limit_product()`` of the shared ``adm_gain_limit.h`` (the SYCL
twin's integer form), on the limit the host splits with
``adm_gain_limit_split()`` and passes as three fields of the CSF uniform, whose
layout the kernel and the host must share.

Device-free: reads the sources only. ``test_metal_integer_adm_math`` holds the
decouple against the CPU on the host; ``test_metal_integer_adm_parity`` compares
the scores on an Apple device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"

MATH = "metal/metal_integer_adm_math.h"
KERNEL = "metal/integer_adm.metal"
HOST = "metal/integer_adm_metal.mm"
SHARED = "adm_gain_limit.h"
REFERENCE = "integer_adm_kernels.h"
TABLE = "integer_adm.h"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
METAL_TYPES = {"int": "int32_t", "uint": "uint32_t", "float": "float"}

MATH_PIECES = {
    "vmaf_mtl_i32 vmaf_mtl_iadm_recip(vmaf_mtl_i32 o)": ("return 1073741824 / o;",),
    "vmaf_mtl_i32 vmaf_mtl_iadm_gain_limit(": (
        "const vmaf_mtl_i64 gained = adm_gain_limit_product(rst, g);",
        "return (vmaf_mtl_i32)((gained < (vmaf_mtl_i64)t) ? gained : (vmaf_mtl_i64)t);",
        "return (vmaf_mtl_i32)((gained > (vmaf_mtl_i64)t) ? gained : (vmaf_mtl_i64)t);",
    ),
    "vmaf_mtl_i32 vmaf_mtl_iadm_decouple_s0(": (
        "const vmaf_mtl_i32 rst = ((k * o) + 16384) >> 15;",
    ),
    "vmaf_mtl_i32 vmaf_mtl_iadm_decouple_s123(": (
        "const vmaf_mtl_i32 rst = (vmaf_mtl_i32)(((k * o) + 16384) >> 15);",
    ),
}
KERNEL_CALLS = {
    "vmaf_mtl_iadm_decouple_s0(o_val, t_val, af, iadm_gain(c))": 3,
    "vmaf_mtl_iadm_decouple_s123(o_val, t_val, af, iadm_gain(c))": 1,
}
HOST_PIECES = (
    '#include "../adm_gain_limit.h"',
    "const struct AdmGainLimit gain = adm_gain_limit_split(s->adm_enhn_gain_limit);",
    "c.gain_m_hi = gain.m_hi;",
    "c.gain_m_lo = gain.m_lo;",
    "c.gain_frac_bits = gain.frac_bits;",
)
REFERENCE_LINES = (
    "const int32_t tmp_k = (o == 0) ? 32768 : (((int64_t)lut[o + 32768] * t) + 16384) >> 15;",
    "rst = ADM_KERNEL_MIN((rst * gain), t);",
    "rst = ADM_KERNEL_MAX((rst * gain), t);",
)
TABLE_LINE = "const int32_t recip = (int32_t)(div_Q_factor / i);"


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(COMMENT.sub(" ", source).split())


def _sources() -> dict[str, str]:
    names = (MATH, KERNEL, HOST, SHARED, REFERENCE, TABLE)
    return {name: (FEATURE / name).read_text(encoding="utf-8") for name in names}


def _function_body(code: str, signature: str) -> str:
    """Flattened text of the definition that starts with `signature` (brace-matched), or empty."""
    start = code.find(signature)
    if start < 0:
        return ""
    depth = 0
    for index in range(code.index("{", start), len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return code[start : index + 1]
    return ""


def _struct_fields(code: str, opener: str, closer: str) -> list[tuple[str, str]]:
    """(type, name) of each member between `opener` and `closer`, Metal types mapped."""
    start = code.find(opener)
    end = code.find(closer, start)
    if start < 0 or end < 0:
        return []
    body = code[code.index("{", start) + 1 : end]
    fields = re.findall(r"\b(\w+) (\w+(?:\[\d+\])?);", body)
    return [(METAL_TYPES.get(kind, kind), name) for kind, name in fields]


def _math_failures(math: str) -> list[str]:
    code = _flat(math)
    failures: list[str] = []
    for signature, pieces in MATH_PIECES.items():
        body = _function_body(code, signature)
        if not body or any(piece not in body for piece in pieces):
            failures.append(f"{MATH}: `{signature}` is not the CPU's integer decouple")
    recip = _function_body(code, "vmaf_mtl_i32 vmaf_mtl_iadm_recip(vmaf_mtl_i32 o)")
    if "float" in recip or "(float)rst" in code:
        failures.append(f"{MATH}: the reciprocal or the gain product is formed in fp32")
    if '#include "../adm_gain_limit.h"' not in code:
        failures.append(f"{MATH}: the decouple does not use the shared adm_gain_limit.h")
    return failures


def _kernel_failures(kernel: str) -> list[str]:
    code = _flat(kernel)
    failures: list[str] = []
    if '#include "metal_integer_adm_math.h"' not in code:
        failures.append(f"{KERNEL}: the kernel does not include the decouple header")
    if any(code.count(call) != count for call, count in KERNEL_CALLS.items()):
        failures.append(f"{KERNEL}: a decouple site does not call the header")
    if re.search(r"IADM_DIV_Q_FACTOR|\begl\b|float gain_limit|iadm_decouple_r_s", code):
        failures.append(f"{KERNEL}: the fp32 reciprocal or the binary32 gain limit is back")
    return failures


def _host_failures(host: str, kernel: str) -> list[str]:
    code = _flat(host)
    failures: list[str] = []
    if any(piece not in code for piece in HOST_PIECES):
        failures.append(f"{HOST}: the host does not split the gain limit for the kernels")
    if "(float)s->adm_enhn_gain_limit" in code:
        failures.append(f"{HOST}: the host narrows the gain limit to float")
    metal = _struct_fields(_flat(kernel), "struct IadmCsf {", "};")
    mm = _struct_fields(code, "typedef struct IadmCsfHost {", "} IadmCsfHost;")
    if not metal or metal != mm:
        failures.append(f"{HOST}: IadmCsfHost and the kernel's IadmCsf differ")
    return failures


def _shared_failures(shared: str) -> list[str]:
    code = COMMENT.sub(" ", shared)
    guards = [m.start() for m in re.finditer(r"#if !defined\(__METAL_VERSION__\)", code)]
    product = code.find("adm_gain_limit_product(int32_t rst")
    split = code.find("adm_gain_limit_split(double gain)")
    if len(guards) != 2 or not guards[0] < guards[1] < split:
        return [f"{SHARED}: the includes and the split are not kept out of Metal"]
    if product < 0 or code.rfind("#endif", 0, product) < split:
        return [f"{SHARED}: adm_gain_limit_product() is not visible to Metal"]
    return []


def _reference_failures(reference: str, table: str) -> list[str]:
    code = _flat(reference)
    failures = [
        f"{REFERENCE} no longer holds `{line}`; the twin mirrors it"
        for line in REFERENCE_LINES
        if line not in code
    ]
    if TABLE_LINE not in _flat(table):
        failures.append(f"{TABLE} no longer holds `{TABLE_LINE}`; the twin mirrors it")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _math_failures(sources[MATH])
        + _kernel_failures(sources[KERNEL])
        + _host_failures(sources[HOST], sources[KERNEL])
        + _shared_failures(sources[SHARED])
        + _reference_failures(sources[REFERENCE], sources[TABLE])
    )


class IntegerAdmMetalExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_fp32_reciprocal_is_detected(self) -> None:
        # The pre-port scale-0 reciprocal.
        failures = self._edited(
            MATH,
            "    return 1073741824 / o;",
            "    return (vmaf_mtl_i32)(1073741824.0f / (float)o);",
        )
        self._assert_detected(failures, "formed in fp32")

    def test_binary32_gain_product_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const vmaf_mtl_i64 gained = adm_gain_limit_product(rst, g);",
            "    const vmaf_mtl_i64 gained = (vmaf_mtl_i64)((float)rst * egl);",
        )
        self._assert_detected(failures, "is not the CPU's integer decouple")
        self._assert_detected(failures, "formed in fp32")

    def test_float_restored_sample_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const vmaf_mtl_i32 rst = (vmaf_mtl_i32)(((k * o) + 16384) >> 15);",
            "    const vmaf_mtl_i32 rst = (vmaf_mtl_i32)(float)(((k * o) + 16384) >> 15);",
        )
        self._assert_detected(failures, "vmaf_mtl_iadm_decouple_s123(")

    def test_kernel_side_decouple_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "vmaf_mtl_iadm_decouple_s123(o_val, t_val, af, iadm_gain(c))",
            "iadm_decouple_r_s123(o_val, t_val, af, c.gain_limit)",
        )
        self._assert_detected(failures, "does not call the header")
        self._assert_detected(failures, "binary32 gain limit is back")

    def test_float_limit_on_the_host_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    const struct AdmGainLimit gain = adm_gain_limit_split(s->adm_enhn_gain_limit);",
            "    const float gain_limit = (float)s->adm_enhn_gain_limit;",
        )
        self._assert_detected(failures, "does not split the gain limit")
        self._assert_detected(failures, "narrows the gain limit to float")

    def test_uniform_layout_drift_is_detected(self) -> None:
        failures = self._edited(HOST, "    int32_t gain_frac_bits;\n", "")
        self._assert_detected(failures, "IadmCsfHost and the kernel's IadmCsf differ")

    def test_unguarded_split_is_detected(self) -> None:
        failures = self._edited(
            SHARED,
            "#if !defined(__METAL_VERSION__) /* Metal has no double: the host splits. */\n",
            "\n",
        )
        self._assert_detected(failures, "kept out of Metal")

    def test_changed_reference_gain_limit_is_detected(self) -> None:
        failures = self._edited(
            REFERENCE,
            "        rst = ADM_KERNEL_MIN((rst * gain), t);",
            "        rst = ADM_KERNEL_MIN((int32_t)(rst * (float)gain), t);",
        )
        self._assert_detected(failures, "the twin mirrors it")

    def test_changed_reciprocal_table_is_detected(self) -> None:
        failures = self._edited(
            TABLE,
            "        const int32_t recip = (int32_t)(div_Q_factor / i);",
            "        const int32_t recip = (int32_t)((float)div_Q_factor / (float)i);",
        )
        self._assert_detected(failures, "the twin mirrors it")


if __name__ == "__main__":
    unittest.main()
