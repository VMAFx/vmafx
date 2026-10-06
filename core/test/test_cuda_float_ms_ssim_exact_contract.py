#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the raster-order per-scale sums of float_ms_ssim_cuda (ADR-1465).

``ms_ssim.c`` runs ``iqa/ssim_tools.c::iqa_ssim()`` once per scale. That
function adds the ``l``, ``c`` and ``s`` term of every window into one
``double`` each, left to right and top to bottom, and returns each mean as a
float. The twin's per-window terms follow the CPU's arithmetic (ADR-1403), so
what can still move a mean is the order of the sum: a per-block sum of the
same terms rounded ``float_ms_ssim_c_scale1`` of the frame in
``float_ms_ssim_order_frame.h`` to the neighbouring float
(T-CUDA-FLOAT-MS-SSIM-FRAME-SUM-ORDER-2026-10-02).

``float_ms_ssim_cuda`` therefore does not reduce the terms on the device:
``ms_ssim_vert_lcs`` stores each window's terms at the window's raster
position, the host reads the three planes of a scale back and
``ms_ssim_scale_sums()`` adds them in index order.

Level 0 of every pyramid is ``picture_copy()`` of the picture plane, computed
on the device by ``ms_ssim_picture_to_float`` with ``picture_copy()``'s
divisors; no picture plane goes to the host and back
(T-CUDA-MS-SSIM-HOST-STAGING-2026-10-06). ``test_cuda_float_ms_ssim_host_traffic``
counts the copies on a device.

Device-free: reads the sources only. Every planted regression below is a
construct the earlier twin had, so the contract fails on that design and passes
on this one. ``test_cuda_float_ms_ssim_order`` checks the bits on a device.
"""

from __future__ import annotations

import hashlib
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CUDA_ROOT = ROOT / "core" / "src" / "feature" / "cuda"

HOST = "integer_ms_ssim_cuda.c"
KERNEL = "integer_ms_ssim/ms_ssim_score.cu"
FIXTURE = ROOT / "core" / "test" / "float_ms_ssim_order_frame.h"
# The frame pair is shared by the HIP, CUDA and SYCL twin tests; the lanes add
# the same file, so its bytes are fixed.
FIXTURE_SHA256 = "be2341f63ce741510d600479c78313b1331b4070a505e3ed0bb1bfc3f30ffff1"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
TERM_STORES = (
    "const size_t window = (size_t)y * w_final + x;",
    "reinterpret_cast<double *>(l_terms.data)[window] = terms.l;",
    "reinterpret_cast<double *>(c_terms.data)[window] = terms.c;",
    "reinterpret_cast<float *>(s_terms.data)[window] = (float)terms.s;",
)
# A shuffle of a double or a shared array of doubles: a device reduction.
DOUBLE_REDUCTION = re.compile(r"__shfl_\w+\s*\(|__shared__\s+double\b")
HOST_SUM = (
    "for (size_t j = 0u; j < n_windows; j++) {",
    "l += l_terms[j];",
    "c += c_terms[j];",
    "st += (double)s_terms[j];",
)
HOST_CALL = (
    "ms_ssim_scale_sums(pl->h_l_terms[i], pl->h_c_terms[i], pl->h_s_terms[i], "
    "pl->scale_window_count[i], sums);"
)
WINDOWS = "pl->scale_window_count[i] = (size_t)pl->scale_w_final[i] * pl->scale_h_final[i];"
READBACKS = (
    "cuMemcpyDtoHAsync(pl->h_l_terms[i], (CUdeviceptr)pl->l_terms[i]->data, "
    "windows * sizeof(double), s->lc.str)",
    "cuMemcpyDtoHAsync(pl->h_c_terms[i], (CUdeviceptr)pl->c_terms[i]->data, "
    "windows * sizeof(double), s->lc.str)",
    "cuMemcpyDtoHAsync(pl->h_s_terms[i], (CUdeviceptr)pl->s_terms[i]->data, "
    "windows * sizeof(float), s->lc.str)",
)


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _flat(source: str) -> str:
    """Comment-free source on one line, so a formatter's wrapping cannot hide a statement."""
    return " ".join(_code(source).split())


def _function_body(code: str, name: str) -> str:
    """The text from the definition of `name` to the closing brace in column 0."""
    start = code.find(f" {name}(")
    if start < 0:
        return ""
    end = code.find("\n}", start)
    return code[start:end] if end >= 0 else ""


def _sources() -> dict[str, str]:
    return {name: (CUDA_ROOT / name).read_text(encoding="utf-8") for name in (HOST, KERNEL)}


def _kernel_failures(kernel: str) -> list[str]:
    failures: list[str] = []
    flat = _flat(kernel)
    for piece in TERM_STORES:
        if piece not in flat:
            failures.append(f"{KERNEL}: a term is not stored at its window's raster position ({piece})")
    if DOUBLE_REDUCTION.search(_code(kernel)):
        failures.append(f"{KERNEL}: the double terms are reduced on the device")
    return failures


def _host_failures(host: str) -> list[str]:
    failures: list[str] = []
    flat = _flat(host)
    sums = " ".join(_function_body(_code(host), "ms_ssim_scale_sums").split())
    for piece in HOST_SUM:
        if piece not in sums:
            failures.append(f"{HOST}: ms_ssim_scale_sums() is not three doubles in raster order ({piece})")
    if sums.count("for (") != 1:
        failures.append(f"{HOST}: ms_ssim_scale_sums() is not one pass in raster order")
    if HOST_CALL not in flat:
        failures.append(f"{HOST}: collect no longer adds a scale's whole term planes")
    if WINDOWS not in flat or any(piece not in flat for piece in READBACKS):
        failures.append(f"{HOST}: the readback is not one term per window")
    if "partials" in flat or "block_count" in flat:
        failures.append(f"{HOST}: the readback holds per-block partial sums")
    return failures


# T-MS-SSIM-GPU-CHROMA-OPTION-DRIFT-2026-09-06: with enable_chroma the twin
# runs the same pipeline once per plane, as float_ms_ssim.c does, and emits the
# CPU's three plane features.
CHROMA_PIECES = (
    "s->n_planes = vmaf_metal_ms_ssim_active_planes(s->enable_chroma, pix_fmt);",
    "const int err = ms_ssim_submit_plane(fex, s, ref_pic, dist_pic, p);",
    "ms_ssim_plane_scores(&s->planes[p], &scores[p]);",
    'static const char *provided_features[] = {"float_ms_ssim", "float_ms_ssim_cb", '
    '"float_ms_ssim_cr", NULL};',
)


def _chroma_failures(host: str) -> list[str]:
    flat = _flat(host)
    return [
        f"{HOST}: the planes enable_chroma scores are not the CPU's ({piece})"
        for piece in CHROMA_PIECES
        if piece not in flat
    ]


# T-CUDA-MS-SSIM-HOST-STAGING-2026-10-06: the twin copied each plane to pinned
# host memory, waited, ran picture_copy() there and uploaded the floats.
LEVEL0_HOST = (
    "CUstream stream = vmaf_cuda_picture_get_stream(ref_pic);",
    "ms_ssim_launch_to_float(s, cu_f, ref_pic, plane, pl->pyramid_ref, stream)",
    "ms_ssim_launch_to_float(s, cu_f, dist_pic, plane, pl->pyramid_cmp, stream)",
    "cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT)",
    # picture_copy()'s divisors (feature/picture_copy.cpp).
    "return bpc == 10u ? 4.0f : bpc == 12u ? 16.0f : bpc == 16u ? 256.0f : 0.0f;",
)
LEVEL0_KERNEL = (
    "__global__ void ms_ssim_picture_to_float(",
    "out[x] = (float)v / scaler;",
    "out[x] = (float)row[x];",
)
HOST_STAGING = re.compile(r"CU_MEMORYTYPE_HOST|cuMemcpyHtoD|picture_copy\s*\(|cuStreamSynchronize")


def _level0_failures(sources: dict[str, str]) -> list[str]:
    host = _flat(sources[HOST])
    kernel = _flat(sources[KERNEL])
    failures = [
        f"{HOST}: level 0 is not converted on the device ({piece})"
        for piece in LEVEL0_HOST
        if piece not in host
    ]
    failures += [
        f"{KERNEL}: level 0 is not picture_copy() on the device ({piece})"
        for piece in LEVEL0_KERNEL
        if piece not in kernel
    ]
    staging = HOST_STAGING.search(_code(sources[HOST]))
    if staging:
        failures.append(f"{HOST}: a picture plane goes through the host ({staging.group(0)})")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _kernel_failures(sources[KERNEL])
        + _host_failures(sources[HOST])
        + _chroma_failures(sources[HOST])
        + _level0_failures(sources)
    )


class FloatMsSsimCudaExactContract(unittest.TestCase):
    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_fixture_is_the_shared_frame(self) -> None:
        digest = hashlib.sha256(FIXTURE.read_bytes()).hexdigest()
        self.assertEqual(digest, FIXTURE_SHA256)

    def test_warp_reduction_of_the_terms_is_detected(self) -> None:
        # The earlier kernel's shuffle loop.
        sources = _sources()
        sources[KERNEL] += (
            "\ndouble r(double wl) { return wl + __shfl_down_sync(0xffffffff, wl, 16u); }\n"
        )
        self.assertTrue(any("reduced on the device" in item for item in _contract_failures(sources)))

    def test_block_partial_array_is_detected(self) -> None:
        # The earlier kernel's `s_l_warp`.
        sources = _sources()
        sources[KERNEL] += "\nvoid f(void) { __shared__ double s_l_warp[4]; }\n"
        self.assertTrue(any("reduced on the device" in item for item in _contract_failures(sources)))

    def test_unstored_term_is_detected(self) -> None:
        sources = _sources()
        sources[KERNEL] = sources[KERNEL].replace(
            "reinterpret_cast<double *>(c_terms.data)[window] = terms.c;", "(void)terms.c;", 1
        )
        self.assertTrue(any("raster position" in item for item in _contract_failures(sources)))

    def test_block_indexed_store_is_detected(self) -> None:
        sources = _sources()
        sources[KERNEL] = sources[KERNEL].replace(
            "const size_t window = (size_t)y * w_final + x;",
            "const size_t window = blockIdx.y * gridDim.x + blockIdx.x;",
            1,
        )
        self.assertTrue(any("raster position" in item for item in _contract_failures(sources)))

    def test_host_sum_of_block_partials_is_detected(self) -> None:
        # The earlier collect_fex_cuda().
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            "pl->scale_window_count[i], sums);", "pl->scale_block_count[i], sums);", 1
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("whole term planes" in item for item in failures))
        self.assertTrue(any("per-block partial sums" in item for item in failures))

    def test_block_sized_readback_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            WINDOWS,
            "pl->scale_window_count[i] = (size_t)pl->scale_grid_x[i] * pl->scale_grid_y[i];",
            1,
        )
        self.assertTrue(any("one term per window" in item for item in _contract_failures(sources)))

    def test_reordered_host_sum_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            "for (size_t j = 0u; j < n_windows; j++) {", "for (size_t j = n_windows; j-- > 0u;) {", 1
        )
        self.assertTrue(any("three doubles in raster" in item for item in _contract_failures(sources)))

    def test_sum_of_another_plane_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace("c += c_terms[j];", "c += l_terms[j];", 1)
        self.assertTrue(any("three doubles in raster" in item for item in _contract_failures(sources)))

    def test_fp32_structure_sum_is_detected(self) -> None:
        # The CPU widens s before it adds it; an fp32 accumulator is another sum.
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            "st += (double)s_terms[j];", "st = (double)((float)st + s_terms[j]);", 1
        )
        self.assertTrue(any("three doubles in raster" in item for item in _contract_failures(sources)))

    def test_luma_only_plane_count_is_detected(self) -> None:
        # The HIP twin's former `n_planes = 1u` whatever enable_chroma said.
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            "s->n_planes = vmaf_metal_ms_ssim_active_planes(s->enable_chroma, pix_fmt);",
            "s->n_planes = 1u;",
            1,
        )
        self.assertTrue(any("enable_chroma scores" in item for item in _contract_failures(sources)))

    def test_luma_only_provided_features_are_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace('"float_ms_ssim_cb", ', "", 1)
        self.assertTrue(any("enable_chroma scores" in item for item in _contract_failures(sources)))

    def test_host_staging_is_detected(self) -> None:
        # The earlier ms_ssim_copy_plane_to_host() / ms_ssim_upload_level_zero().
        planted = (
            "\nstatic int f(void) { CUDA_MEMCPY2D c = {.dstMemoryType = CU_MEMORYTYPE_HOST}; }\n",
            "\nstatic void f(void) { cuMemcpyHtoDAsync(0, 0, 0, 0); }\n",
            "\nstatic void f(void) { picture_copy(0, 0, 0, 0, 8, 0); }\n",
            "\nstatic void f(void) { cuStreamSynchronize(0); }\n",
        )
        for code in planted:
            sources = _sources()
            sources[HOST] += code
            self.assertTrue(
                any("goes through the host" in item for item in _contract_failures(sources)), code
            )

    def test_unconverted_level0_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace(
            "ms_ssim_launch_to_float(s, cu_f, dist_pic, plane, pl->pyramid_cmp, stream)", "0", 1
        )
        self.assertTrue(
            any("not converted on the device" in item for item in _contract_failures(sources))
        )

    def test_other_divisor_is_detected(self) -> None:
        sources = _sources()
        sources[HOST] = sources[HOST].replace("bpc == 10u ? 4.0f", "bpc == 10u ? 1024.0f", 1)
        self.assertTrue(
            any("not converted on the device" in item for item in _contract_failures(sources))
        )

    def test_kernel_without_division_is_detected(self) -> None:
        sources = _sources()
        sources[KERNEL] = sources[KERNEL].replace(
            "out[x] = (float)v / scaler;", "out[x] = (float)v;", 1
        )
        self.assertTrue(
            any("not picture_copy() on the device" in item for item in _contract_failures(sources))
        )


if __name__ == "__main__":
    unittest.main()
