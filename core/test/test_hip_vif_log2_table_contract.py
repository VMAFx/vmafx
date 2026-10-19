#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin vif_hip's logarithms to the CPU's table (ADR-1435).

``integer_vif.c`` takes every per-pixel logarithm from a 32768-entry table it
fills at init with the host math library:
``round(log2f(32768 + i) * 2048)``. ``vif_hip`` accumulates the same integers
as the CPU, so its scores are the CPU's bit for bit exactly when its kernels
read the same table. They evaluated ``log2f()`` on the device instead, which
is one ulp from glibc's on about half of the arguments and moved 77 of the
32768 entries by one: nearly every frame was up to 5.4e-7 from the CPU.

The table has one definition, ``vif_log2_table_generate()`` in
``vif_log2_table.h``, which ``integer_vif.h`` includes. The CPU extractor
fills its state with it, the HIP host uploads its values, and the kernels
look them up. The SYCL and Metal hosts build their device tables with the
same call: they computed the same values with copies of the expression, and a
copy can drift.

Device-free: reads the sources only. ``test_hip_vif_parity`` compares the
scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

CPU = "integer_vif.c"
CPU_HEADER = "vif_log2_table.h"
VIF_HEADER = "integer_vif.h"
SYCL_HOST = "sycl/integer_vif_sycl.cpp"
METAL_HOST = "metal/integer_vif_metal.mm"
HOST = "hip/integer_vif_hip.c"
HOST_HEADER = "hip/integer_vif_hip.h"
KERNEL = "hip/integer_vif/vif_statistics.hip"

TABLE_ENTRY = "log2_table[i] = (uint16_t)roundf(log2f((float)(VIF_LOG2_TABLE_OFFSET + i)) * 2048);"
# A logarithm, or the rounding that goes with one, evaluated where the table
# should be read.
DEVICE_LOG = re.compile(r"\b(?:log2f?|logf?|log10f?|__float2int_rn|roundf?|lroundf?)\s*\(")
# The three logarithms of one pixel: the denominator's and the two of the
# numerator.
LOOKUPS_PER_PIXEL = 3
# The 8-bit scale-0 launch and the 16-bit launch of every other case.
HORIZONTAL_LAUNCHES = 2

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
SPACE = re.compile(r"\s+")


def _code(source: str) -> str:
    """The source without comments and with whitespace collapsed."""
    return SPACE.sub(" ", COMMENT.sub(" ", source))


def _sources() -> dict[str, str]:
    names = (CPU, CPU_HEADER, VIF_HEADER, HOST, HOST_HEADER, KERNEL, SYCL_HOST, METAL_HOST)
    return {name: (FEATURE_ROOT / name).read_text(encoding="utf-8") for name in names}


def _function_body(source: str, name: str) -> str:
    """Text of the first definition of `name` (brace-matched), or empty."""
    match = re.search(rf"\b{name}\([^;{{]*\)\s*\{{", source)
    if not match:
        return ""
    depth = 0
    for index in range(match.end() - 1, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : index + 1]
    return ""


def _cpu_failures(sources: dict[str, str]) -> list[str]:
    """One definition of the table, and the CPU extractor uses it."""
    failures: list[str] = []
    header = _code(sources[CPU_HEADER])
    if TABLE_ENTRY not in _function_body(header, "vif_log2_table_generate"):
        failures.append(f"{CPU_HEADER}: vif_log2_table_generate() no longer builds the table")
    cpu = _code(sources[CPU])
    if "vif_log2_table_generate(s->public.log2_table);" not in _function_body(cpu, "init"):
        failures.append(f"{CPU}: init() must fill the table with vif_log2_table_generate()")
    if "log2f(" in cpu:
        failures.append(f"{CPU}: a second definition of the log2 table")
    vif_header = _code(sources[VIF_HEADER])
    if '#include "vif_log2_table.h"' not in vif_header or "log2f(" in vif_header:
        failures.append(f"{VIF_HEADER}: the table is not the one of {CPU_HEADER}")
    return failures


def _other_twin_failures(sources: dict[str, str]) -> list[str]:
    """The SYCL and Metal hosts fill their device tables with the CPU's generator."""
    failures: list[str] = []
    for name in (SYCL_HOST, METAL_HOST):
        code = _code(sources[name])
        if "vif_log2_table_generate(" not in code:
            failures.append(f"{name}: the device table is not vif_log2_table_generate()'s")
        if re.search(r"\blog2f?\s*\(", code):
            failures.append(f"{name}: a second definition of the log2 table")
        if "#define VIF_LOG2_TABLE_SIZE" in code:
            failures.append(f"{name}: a second definition of the table size")
    return failures


def _kernel_failures(sources: dict[str, str]) -> list[str]:
    kernel = _code(sources[KERNEL])
    failures: list[str] = []
    if DEVICE_LOG.search(kernel):
        failures.append(f"{KERNEL}: a logarithm evaluated on the device; read the CPU's table")
    lookup = _function_body(kernel, "log2_lookup")
    if "return (int32_t)log2_table[v & (VIF_HIP_LOG2_TABLE_SIZE - 1u)];" not in lookup:
        failures.append(f"{KERNEL}: log2_lookup() must index the table as log2_32() does")
    statistic = _function_body(kernel, "vif_statistic_log_domain")
    if statistic.count("log2_lookup(log2_table, ") != LOOKUPS_PER_PIXEL:
        failures.append(f"{KERNEL}: every logarithm of a pixel is a lookup in the CPU's table")
    return failures


def _host_failures(sources: dict[str, str]) -> list[str]:
    host = _code(sources[HOST])
    failures: list[str] = []
    upload = _function_body(host, "vif_hip_tables_upload")
    if "vif_log2_table_generate(log2_table);" not in upload:
        failures.append(f"{HOST}: the uploaded table must come from vif_log2_table_generate()")
    if "hipMemcpy(s->log2_table_dev, log2_table, table_sz, hipMemcpyHostToDevice)" not in upload:
        failures.append(f"{HOST}: the CPU's table is not uploaded")
    if DEVICE_LOG.search(host):
        failures.append(f"{HOST}: a second definition of the log2 table")
    if host.count("(void *)&log2_table_dev,") != HORIZONTAL_LAUNCHES:
        failures.append(f"{HOST}: both horizontal launches pass the table to the kernel")
    if "_Static_assert(VIF_HIP_LOG2_TABLE_SIZE == VIF_LOG2_TABLE_SIZE," not in host:
        failures.append(f"{HOST}: the kernel's table size is not checked against the CPU's")
    if "#define VIF_HIP_LOG2_TABLE_SIZE 32768u" not in sources[HOST_HEADER]:
        failures.append(f"{HOST_HEADER}: VIF_HIP_LOG2_TABLE_SIZE is not the CPU's table size")
    return failures


def _failures(sources: dict[str, str]) -> list[str]:
    return (
        _cpu_failures(sources)
        + _kernel_failures(sources)
        + _host_failures(sources)
        + _other_twin_failures(sources)
    )


class HipVifLog2TableContractTest(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _failures(sources)

    def test_live_sources_read_the_cpu_table(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_device_logarithm_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    return (int32_t)log2_table[v & (VIF_HIP_LOG2_TABLE_SIZE - 1u)];",
            "    (void)log2_table;\n    return (int32_t)__float2int_rn(log2f((float)v) * 2048.0f);",
        )
        self.assertTrue(any("evaluated on the device" in failure for failure in failures), failures)

    def test_unmasked_lookup_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "log2_table[v & (VIF_HIP_LOG2_TABLE_SIZE - 1u)]",
            "log2_table[v]",
        )
        self.assertTrue(any("as log2_32() does" in failure for failure in failures), failures)

    def test_host_table_of_its_own_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    vif_log2_table_generate(log2_table);",
            "    for (unsigned i = 0; i < VIF_LOG2_TABLE_SIZE; ++i)\n"
            "        log2_table[i] = (uint16_t)lroundf(log2f((float)(32768u + i)) * 2048.0f);",
        )
        self.assertTrue(
            any("vif_log2_table_generate()" in failure for failure in failures), failures
        )
        self.assertTrue(any("second definition" in failure for failure in failures), failures)

    def test_launch_without_the_table_is_detected(self) -> None:
        failures = self._edited(HOST, "                         (void *)&log2_table_dev,\n", "")
        self.assertTrue(
            any("both horizontal launches" in failure for failure in failures), failures
        )

    def test_cpu_table_of_its_own_is_detected(self) -> None:
        failures = self._edited(
            CPU,
            "    vif_log2_table_generate(s->public.log2_table);",
            "    for (unsigned i = 0; i < VIF_LOG2_TABLE_SIZE; ++i)\n"
            "        s->public.log2_table[i] = (uint16_t)round(log2f((float)(32768u + i)) * 2048);",
        )
        self.assertTrue(
            any("init() must fill the table" in failure for failure in failures), failures
        )
        self.assertTrue(
            any(f"{CPU}: a second definition" in failure for failure in failures), failures
        )

    def test_sycl_table_of_its_own_is_detected(self) -> None:
        # The pre-reuse vif_init_log2_lut().
        failures = self._edited(
            SYCL_HOST,
            "    vif_log2_table_generate(table);\n",
            "    for (int j = 0; j < LOG2_LUT_SIZE; j++)\n"
            "        table[j] = static_cast<uint16_t>(\n"
            "            std::roundf(std::log2f((float)(j + 32768)) * 2048.0f));\n",
        )
        self.assertTrue(
            any(f"{SYCL_HOST}: the device table" in item for item in failures), failures
        )
        self.assertTrue(any(f"{SYCL_HOST}: a second definition" in item for item in failures))

    def test_metal_table_of_its_own_is_detected(self) -> None:
        # The pre-reuse fill_log2_table().
        failures = self._edited(
            METAL_HOST,
            "    vif_log2_table_generate((uint16_t *)[(__bridge id<MTLBuffer>)s->log2_buf contents]);",
            "    for (unsigned i = 0; i < VIF_LOG2_TABLE_SIZE; ++i)\n"
            "        ((uint16_t *)[(__bridge id<MTLBuffer>)s->log2_buf contents])[i] =\n"
            "            (uint16_t)lround(log2f((float)(0x8000u + i)) * 2048.0f);",
        )
        self.assertTrue(
            any(f"{METAL_HOST}: the device table" in item for item in failures), failures
        )
        self.assertTrue(any(f"{METAL_HOST}: a second definition" in item for item in failures))


if __name__ == "__main__":
    unittest.main()
