#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin vif_cuda's logarithms to the CPU's table (ADR-1462).

The CPU ``vif`` reads its logarithms from a table that
``vif_log2_table_generate()`` fills with the host math library. ``vif_cuda``
reads the same table on the device: a global of its kernel module that the
host uploads at init. It evaluates no logarithm on the device, whose
``log2f()`` need not round as the host's does (ADR-1435: 77 of 32768 entries
differed on an AMD device).

This test reads the sources:

- the statistic takes every logarithm from ``log2_lookup()``, which indexes
  the module's table with the CPU's mask;
- no kernel source of the twin calls a logarithm or a rounding function;
- the host generates the table with ``vif_log2_table_generate()``, stages it
  on the device and copies it into the module's global with the transfer
  kernel, waiting for it, before init returns, so before any frame is
  submitted.

Device-free. ``test_cuda_vif_log2_table`` runs the upload on a device and
compares all entries; ``test_cuda_vif_parity`` compares the scores.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"

STATISTIC = "cuda/integer_vif/vif_statistics.cuh"
KERNELS = "cuda/integer_vif/filter1d.cu"
HOST = "cuda/integer_vif_cuda.c"
HEADER = "cuda/integer_vif_cuda.h"
TABLE = "vif_log2_table.h"
BUILD = ROOT / "core" / "src" / "meson.build"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)

TABLE_GLOBAL = "__device__ uint16_t vif_cuda_log2_table[VIF_LOG2_TABLE_SIZE];"
LOOKUP = "return vif_cuda_log2_table[v & (VIF_LOG2_TABLE_SIZE - 1u)];"
STATISTIC_SITES = (
    "den_val = log2_lookup(log_den1);",
    "num_val = log2_lookup(numlog) - log2_lookup(denlog);",
)
TRANSFER_KERNEL = (
    "if (i >= VIF_LOG2_TABLE_SIZE) { return; }",
    "if (to_module != 0u) { vif_cuda_log2_table[i] = staged[i]; }",
    "else { staged[i] = vif_cuda_log2_table[i]; }",
)
UPLOAD = (
    "const size_t table_bytes = VIF_LOG2_TABLE_SIZE * sizeof(uint16_t);",
    "vif_log2_table_generate(table);",
    "err = vmaf_cuda_buffer_alloc(cu_state, &staging, table_bytes);",
    "err = vmaf_cuda_buffer_upload_async(cu_state, staging, table, 0);",
    "err = vmaf_cuda_vif_log2_table_transfer(cu_state, module, staging, true);",
)
TRANSFER_HOST = (
    'cuModuleGetFunction(&transfer, module, "vif_cuda_log2_table_transfer")',
    "res = cu_f->cuStreamSynchronize(cu_state->str);",
)
INIT_ORDER = (
    "const int cuda_err = vif_init_cuda_context(fex, s, cu_f);",
    "vmaf_cuda_vif_upload_log2_table(fex->cu_state, s->filter1d_module);",
    "return vif_init_unwind(fex, s, table_err);",
    "return vif_setup_buffers(",
)
TABLE_EXPRESSION = (
    "log2_table[i] = (uint16_t)roundf(log2f((float)(VIF_LOG2_TABLE_OFFSET + i)) * 2048);"
)
# A logarithm, a power or a rounding call in device code.
DEVICE_MATH = re.compile(r"\b(?:log(?:2|10|1p)?f?|roundf?|rintf?|__float2int_r[nzud])\s*\(")


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(COMMENT.sub(" ", source).split())


def _sources() -> dict[str, str]:
    sources = {
        name: (FEATURE / name).read_text(encoding="utf-8")
        for name in (STATISTIC, KERNELS, HOST, HEADER, TABLE)
    }
    sources["build"] = BUILD.read_text(encoding="utf-8")
    return sources


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


def _in_order(code: str, pieces: tuple[str, ...]) -> bool:
    position = 0
    for piece in pieces:
        position = code.find(piece, position)
        if position < 0:
            return False
        position += len(piece)
    return True


def _kernel_failures(sources: dict[str, str]) -> list[str]:
    statistic = _flat(sources[STATISTIC])
    failures: list[str] = []
    if TABLE_GLOBAL not in statistic:
        failures.append(f"{STATISTIC}: the module has no log2 table of the CPU table's size")
    if LOOKUP not in _function_body(statistic, "uint16_t log2_lookup(uint16_t v)"):
        failures.append(f"{STATISTIC}: log2_lookup() does not read the table with the CPU's mask")
    if any(site not in statistic for site in STATISTIC_SITES):
        failures.append(f"{STATISTIC}: the statistic does not take each logarithm from the table")
    transfer = _function_body(statistic, "void vif_cuda_log2_table_transfer(")
    if any(piece not in transfer for piece in TRANSFER_KERNEL):
        failures.append(f"{STATISTIC}: the transfer kernel does not copy the whole table")
    for name in (STATISTIC, KERNELS):
        found = DEVICE_MATH.search(_flat(sources[name]))
        if found:
            failures.append(f"{name}: device code evaluates {found.group(0)}...) itself")
    if "vif_log2_probe" in sources["build"]:
        failures.append("core/src/meson.build: the probe fatbin is back; nothing uses it")
    return failures


def _host_failures(sources: dict[str, str]) -> list[str]:
    host = _flat(sources[HOST])
    failures: list[str] = []
    upload = _function_body(host, "int vmaf_cuda_vif_upload_log2_table(")
    if not _in_order(upload, UPLOAD):
        failures.append(f"{HOST}: the upload is not the CPU's table staged and transferred")
    transfer = _function_body(host, "int vmaf_cuda_vif_log2_table_transfer(")
    if not _in_order(transfer, TRANSFER_HOST):
        failures.append(f"{HOST}: the transfer does not wait for its kernel")
    init = _function_body(host, "static int init_fex_cuda(")
    if not _in_order(init, INIT_ORDER):
        failures.append(f"{HOST}: init does not upload the table before it returns")
    if TABLE_EXPRESSION not in _flat(sources[TABLE]):
        failures.append(f"{TABLE}: the one definition of the table changed")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return _kernel_failures(sources) + _host_failures(sources)


class CudaVifLog2Contract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_device_logarithm_is_detected(self) -> None:
        # The twin before ADR-1462: equal to the CPU's table for one pair of
        # device and host math library.
        failures = self._edited(
            STATISTIC,
            "return vif_cuda_log2_table[v & (VIF_LOG2_TABLE_SIZE - 1u)];",
            "return (uint16_t)roundf(log2f(float(v)) * 2048.f);",
        )
        self._assert_detected(failures, "does not read the table")
        self._assert_detected(failures, "device code evaluates")

    def test_logarithm_in_the_statistic_is_detected(self) -> None:
        failures = self._edited(
            STATISTIC,
            "den_val = log2_lookup(log_den1);",
            "den_val = (uint16_t)__float2int_rn(log2f((float)log_den1) * 2048.f);",
        )
        self._assert_detected(failures, "does not take each logarithm from the table")
        self._assert_detected(failures, "device code evaluates")

    def test_unmasked_lookup_is_detected(self) -> None:
        # The index is a mantissa of 32768 to 65535; the table has 32768 entries.
        failures = self._edited(
            STATISTIC,
            "return vif_cuda_log2_table[v & (VIF_LOG2_TABLE_SIZE - 1u)];",
            "return vif_cuda_log2_table[v];",
        )
        self._assert_detected(failures, "CPU's mask")

    def test_host_copy_of_the_expression_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    vif_log2_table_generate(table);",
            "    for (unsigned i = 0; i < VIF_LOG2_TABLE_SIZE; i++)\n"
            "        table[i] = (uint16_t)round(log2(32768.0 + i) * 2048.0);",
        )
        self._assert_detected(failures, "CPU's table staged and transferred")

    def test_partial_transfer_is_detected(self) -> None:
        failures = self._edited(
            STATISTIC,
            "    if (i >= VIF_LOG2_TABLE_SIZE) {\n        return;",
            "    if (i >= VIF_LOG2_TABLE_SIZE / 2u) {\n        return;",
        )
        self._assert_detected(failures, "does not copy the whole table")

    def test_transfer_without_a_wait_is_detected(self) -> None:
        # The staged table and the staging buffer are released right after.
        failures = self._edited(
            HOST,
            "        res = cu_f->cuStreamSynchronize(cu_state->str);",
            "        res = CUDA_SUCCESS;",
        )
        self._assert_detected(failures, "does not wait for its kernel")

    def test_init_without_the_upload_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "vmaf_cuda_vif_upload_log2_table(fex->cu_state, s->filter1d_module);",
            "0;",
        )
        self._assert_detected(failures, "init does not upload the table")

    def test_returning_probe_fatbin_is_detected(self) -> None:
        sources = _sources()
        sources["build"] += "\n        'vif_log2_probe' : ['x.cu'],\n"
        self._assert_detected(_contract_failures(sources), "probe fatbin is back")


if __name__ == "__main__":
    unittest.main()
