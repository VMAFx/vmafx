#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact adm_cuda design (ADR-1416).

``adm_cuda`` returns the CPU ``adm`` extractor's values bit for bit because
three things are the CPU's own and not a copy of them:

- the CSF weights: ``adm_csf_factors()`` of ``integer_adm_kernels.h``. The
  host used to carry a copy of ``dwt_quant_step()`` that evaluated the CSF
  exponent in ``float`` where the CPU evaluates it in ``double``;
- the denominator fold: one rounding shift per row
  (``adm_csf_den_round_row_total()``), with the shifts taken from the CPU's
  context initialisers. The kernels used to fold each warp of a row and to
  derive the shifts with the device ``__log2f``;
- the float conclusion of a scale: ``adm_cm_result()`` /
  ``i4_adm_cm_result()`` / ``adm_csf_den_result()`` /
  ``i4_adm_csf_den_result()``, including the seeds ``adm_skip_scale0`` leaves.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-ADR-1416 twin had. ``test_adm_cm_row_rounding`` checks the
fold on raw accumulators and ``test_cuda_adm_parity`` checks the scores on a
device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

HOST = "cuda/integer_adm_cuda.c"
DEN_KERNEL = "cuda/integer_adm/adm_csf_den.cu"
CPU_KERNELS = "integer_adm_kernels.h"
ACCUMULATOR = "adm_cm_accumulator.h"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
CPU_HEADER_INCLUDE = '#include "feature/integer_adm_kernels.h"'
# A definition, not a call: `static ... name(` at the start of a line.
LOCAL_COPY = re.compile(
    r"^static\s+(?:inline\s+)?[\w\s\*]+?\b"
    r"(dwt_quant_step|adm_csf_factors|conclude_adm_cm|conclude_adm_csf_den)\s*\(",
    re.M,
)
CPU_RESULTS = (
    "adm_cm_result(&c, &bd, s0_accum, noise_weight, s->adm_p_norm)",
    "i4_adm_cm_result(&c, &bd, accum, noise_weight, s->adm_p_norm)",
    "adm_csf_den_result(&c, accum, s->adm_noise_weight)",
    "i4_adm_csf_den_result(&c, accum, s->adm_noise_weight)",
)
# The viewing distance is an argument since the twin evaluates one or two
# distances per frame (ADR-2795): the launch and the result take the same one.
CPU_DEN_CONTEXTS = (
    "adm_csf_den_ctx_init(&c, w, h, nvd",
    "i4_adm_csf_den_ctx_init(&c, scale, w, h, nvd",
)
SKIP_SCALE0_SEED = "float den_scale = (float)1e-10;"
DEVICE_LOG2 = re.compile(r"__log2f\s*\(|\blog2f?\s*\(")
ROW_FOLD = "(uint64_cu)adm_csf_den_round_row_total(row_total, add_shift_accum, shift_accum)"
ROW_LOOP = "for (int j = left + (int)threadIdx.x; j < right; j += ADM_CSF_DEN_THREADS)"
ROW_LAUNCHES = (
    "cuLaunchKernel(s->func_adm_csf_den_scale_row_kernel, 1, (unsigned)rows,",
    "cuLaunchKernel(s->func_adm_csf_den_s123_row_kernel, 1, (unsigned)rows,",
)
CPU_FOLD = "accum[k] += adm_csf_den_round_row_total(inner[k], add_shift_accum, shift_accum);"


MIN_CALLS = 2  # calls of one helper a kernel must keep
ROW_LOOP_COUNT = 2  # row loops the denominator kernel keeps


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _flat(source: str) -> str:
    """Comment-free source with every whitespace run collapsed to one space."""
    return re.sub(r"\s+", " ", _code(source))


def _calls(flat: str, call: str) -> int:
    """Occurrences of ``call`` that are not the tail of a longer identifier."""
    return len(re.findall(r"(?<![A-Za-z0-9_])" + re.escape(call), flat))


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE_ROOT / name).read_text(encoding="utf-8")
        for name in (HOST, DEN_KERNEL, CPU_KERNELS, ACCUMULATOR)
    }


def _host_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    code = _code(sources[HOST])
    flat = _flat(sources[HOST])
    if CPU_HEADER_INCLUDE not in code:
        failures.append(f"{HOST}: the CPU's integer_adm_kernels.h is no longer included")
    for match in LOCAL_COPY.finditer(code):
        failures.append(f"{HOST}: local copy of {match.group(1)}() next to the CPU's routine")
    for call in CPU_RESULTS:
        if not _calls(flat, call):
            failures.append(f"{HOST}: a scale is no longer concluded by {call.split('(')[0]}()")
    for call in CPU_DEN_CONTEXTS:
        if _calls(flat, call) < MIN_CALLS:
            failures.append(
                f"{HOST}: {call.split('(')[0]}() no longer feeds both the launch and the result"
            )
    if SKIP_SCALE0_SEED not in code:
        failures.append(
            f"{HOST}: adm_skip_scale0 no longer seeds the denominator with the CPU's float 1e-10"
        )
    for launch in ROW_LAUNCHES:
        if launch not in flat:
            failures.append(f"{HOST}: a denominator launch is no longer one block per row")
    return failures


def _kernel_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    code = _code(sources[DEN_KERNEL])
    flat = _flat(sources[DEN_KERNEL])
    if DEVICE_LOG2.search(code):
        failures.append(f"{DEN_KERNEL}: a rounding shift is derived on the device")
    if flat.count(ROW_FOLD) != 1:
        failures.append(f"{DEN_KERNEL}: the row total is no longer folded once, by the helper")
    if flat.count("atomicAdd(") != 1:
        failures.append(f"{DEN_KERNEL}: more than one accumulator update per row")
    fold = flat.find("atomicAdd(")
    guard = flat.rfind("if (threadIdx.x == 0)", 0, fold)
    sync = flat.rfind("__syncthreads();", 0, fold)
    if fold < 0 or guard < 0 or sync < 0 or sync > guard:
        failures.append(
            f"{DEN_KERNEL}: the fold is no longer thread 0's, after the block has been reduced"
        )
    if flat.count(ROW_LOOP) != ROW_LOOP_COUNT:
        failures.append(f"{DEN_KERNEL}: a block no longer covers a whole row of its band")
    return failures


def _shared_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    if CPU_FOLD not in _flat(sources[CPU_KERNELS]):
        failures.append(f"{CPU_KERNELS}: adm_csf_den_fold() no longer folds through the helper")
    if "return (row_sum + add_shift_accum) >> shift_accum;" not in _code(sources[ACCUMULATOR]):
        failures.append(f"{ACCUMULATOR}: the row fold is no longer one rounded shift")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return _host_failures(sources) + _kernel_failures(sources) + _shared_failures(sources)


class AdmCudaExactContract(unittest.TestCase):
    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_local_csf_weight_copy_is_detected(self) -> None:
        # The pre-ADR-1416 host: its own dwt_quant_step(), exponent in float.
        sources = _sources()
        sources[HOST] += (
            "\nstatic inline float dwt_quant_step(const struct dwt_model_params *params,"
            " int lambda)\n{\n    return params->k * lambda;\n}\n"
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("local copy of dwt_quant_step" in item for item in failures))

    def test_missing_cpu_header_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace(CPU_HEADER_INCLUDE, "", 1)
        self.assertTrue(any("no longer included" in item for item in _contract_failures(sources)))

    def test_own_conclusion_is_detected(self) -> None:
        # The pre-ADR-1416 conclude_adm_csf_den().
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            "return adm_csf_den_result(&c, accum, s->adm_noise_weight);",
            "return conclude_adm_csf_den(accum, h, w, scale);",
            1,
        )
        sources[HOST] += (
            "\nstatic float conclude_adm_csf_den(const uint64_t *accum, int h, int w, int scale)\n"
            "{\n    return (float)accum[0];\n}\n"
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("adm_csf_den_result()" in item for item in failures))
        self.assertTrue(any("local copy of conclude_adm_csf_den" in item for item in failures))

    def test_double_skip_scale0_seed_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace(SKIP_SCALE0_SEED, "double den_scale = 1e-10;", 1)
        self.assertTrue(any("float 1e-10" in item for item in _contract_failures(sources)))

    def test_per_warp_fold_is_detected(self) -> None:
        # The pre-ADR-1416 kernels: every warp leader rounded its own partial.
        sources = _sources()
        sources[DEN_KERNEL] = sources[DEN_KERNEL].replace(
            "const uint64_cu lane_sum = (uint64_cu)warp_reduce((int64_t)thread_sum);",
            "const uint64_cu lane_sum = (uint64_cu)warp_reduce((int64_t)thread_sum);\n"
            "    if ((threadIdx.x % VMAF_CUDA_THREADS_PER_WARP) == 0)\n"
            "        atomicAdd(reinterpret_cast<uint64_cu *>(band_accum),\n"
            "                  (lane_sum + add_shift_accum) >> shift_accum);",
            1,
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("more than one accumulator update" in item for item in failures))

    def test_device_log2_shift_is_detected(self) -> None:
        sources = _sources()
        sources[DEN_KERNEL] = sources[DEN_KERNEL].replace(
            "uint64_cu thread_sum = 0;",
            "uint32_t shift = __float2uint_ru(__log2f((float)(right - left)));\n"
            "    uint64_cu thread_sum = shift;",
            1,
        )
        self.assertTrue(
            any("derived on the device" in item for item in _contract_failures(sources))
        )

    def test_block_per_row_chunk_launch_is_detected(self) -> None:
        # The pre-ADR-1416 launch: several blocks per row, each folding alone.
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            "cuLaunchKernel(s->func_adm_csf_den_s123_row_kernel, 1, (unsigned)rows,",
            "cuLaunchKernel(s->func_adm_csf_den_s123_row_kernel, blocks_per_row, (unsigned)rows,",
            1,
        )
        self.assertTrue(any("one block per row" in item for item in _contract_failures(sources)))

    def test_cpu_fold_bypassing_the_helper_is_detected(self) -> None:
        sources = _sources()
        sources[CPU_KERNELS] = sources[CPU_KERNELS].replace(
            "adm_csf_den_round_row_total(inner[k], add_shift_accum, shift_accum)",
            "(inner[k] + add_shift_accum) >> shift_accum",
            1,
        )
        self.assertTrue(
            any(
                "no longer folds through the helper" in item for item in _contract_failures(sources)
            )
        )


if __name__ == "__main__":
    unittest.main()
