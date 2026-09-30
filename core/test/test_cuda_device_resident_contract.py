#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the device-resident CUDA CAMBI (ADR-1379) and SpEED (ADR-1380) designs.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-ADR-1379 / pre-ADR-1380 host-residual code actually had, so
the contract fails on the old design and passes on the new one.
"""

from __future__ import annotations

import re
import struct
import unittest
from decimal import Decimal, getcontext
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CUDA_ROOT = ROOT / "core" / "src" / "feature" / "cuda"
MESON_BUILD = ROOT / "core" / "src" / "meson.build"
LOG2_HARD_CASES = ROOT / "core" / "src" / "feature" / "speed_log2_hard_cases.h"

CAMBI_HOST = "integer_cambi_cuda.c"
CAMBI_KERNELS = "integer_cambi/cambi_score.cu"
SPEED_HOSTS = ("speed_chroma_cuda.c", "speed_temporal_cuda.c", "speed_cuda_pipeline.c")
SPEED_KERNELS = "speed/speed_score.cu"

# Calls, not mentions: the sources cite the retired helpers in comments.
CAMBI_HOST_RESIDUAL = tuple(
    re.compile(rf"\b{name}\(\s*[\w&*]")
    for name in (
        "vmaf_cambi_calculate_c_values",
        "vmaf_cambi_spatial_pooling",
        "vmaf_cambi_preprocessing",
        "vmaf_cambi_filter_mode",
        "vmaf_cambi_decimate",
        "vmaf_cuda_picture_download_async",
    )
)
SPEED_HOST_RESIDUAL = tuple(
    re.compile(rf"\b{name}\(\s*[\w&*]")
    for name in (
        "speed_internal_compute_eigenvalues",
        "speed_internal_qr_factorize",
        "speed_internal_qt_multiply",
        "speed_internal_filter_and_downscale",
        "speed_internal_compute_means",
        "speed_internal_compute_cov_matrix",
        "speed_internal_is_matrix_regular",
        "speed_internal_backward_substitution",
        "picture_copy",
    )
)
# Host waits and synchronous copies. The frame's one wait is
# vmaf_cuda_kernel_collect_wait(), at collect time.
HOST_WAIT = re.compile(
    r"\bcu(?:StreamSynchronize|CtxSynchronize|EventSynchronize|MemcpyDtoH|Memcpy2D|MemcpyDtoD)\s*\("
)
COLLECT_WAIT = re.compile(r"\bvmaf_cuda_kernel_collect_wait\s*\(")
# The frame's one device-to-host copy; its size argument names the result
# block, so neither a plane nor per-block data can come back to be combined
# on the host.
READBACK = re.compile(r"\bcuMemcpyDtoH(?:Async)?\s*\(([^;]*);")
GLOBAL_KERNEL = re.compile(
    r"__global__\s+void\s+(?:__launch_bounds__\([^)]*\)\s+)?(\w+)\s*\(([^)]*)\)"
)


def _read(relative: str) -> str:
    return (CUDA_ROOT / relative).read_text(encoding="utf-8")


def _sources() -> dict[str, str]:
    names = (CAMBI_HOST, CAMBI_KERNELS, *SPEED_HOSTS, SPEED_KERNELS)
    sources = {name: _read(name) for name in names}
    sources["meson.build"] = MESON_BUILD.read_text(encoding="utf-8")
    return sources


# A top-level C function definition starts at column 0 with its return type;
# statements inside a body are indented.
DEFINITION = re.compile(r"^(?:static\s+)?[A-Za-z_][\w \t*]*?\b(\w+)\s*\(", re.M)
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _enclosing_function(source: str, pos: int) -> str:
    names = [match.group(1) for match in DEFINITION.finditer(source, 0, pos)]
    return names[-1] if names else ""


def _single_struct_kernels(source: str, file: str, arg_type: str) -> list[str]:
    failures: list[str] = []
    kernels = GLOBAL_KERNEL.findall(source)
    if not kernels:
        failures.append(f"{file}: no __global__ kernel found")
    for name, params in kernels:
        if not re.fullmatch(rf"\s*const\s+{arg_type}\s+a\s*", params):
            failures.append(f"{file}: {name} does not take one `const {arg_type} a` argument")
    return failures


def _waits_outside(source: str, file: str, allowed_fn: str) -> list[str]:
    failures: list[str] = []
    code = _code(source)
    if HOST_WAIT.search(code):
        failures.append(f"{file}: host wait or synchronous copy in the per-frame path")
    for match in COLLECT_WAIT.finditer(code):
        owner = _enclosing_function(code, match.start())
        if owner != allowed_fn:
            failures.append(f"{file}: collect wait in {owner}(), outside {allowed_fn}()")
    return failures


def _readback_failures(source: str, file: str, result_type: str | None) -> list[str]:
    """`result_type`: the one block the file may copy back per frame, or None
    when the file must not copy anything back."""
    copies = READBACK.findall(_code(source))
    if result_type is None:
        return [f"{file}: device-to-host copy outside the pipeline"] if copies else []
    if len(copies) != 1:
        return [f"{file}: {len(copies)} device-to-host copies per frame, expected one"]
    if f"sizeof({result_type})" not in copies[0]:
        return [f"{file}: the readback is not the {result_type} block"]
    return []


def _cambi_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    host = sources[CAMBI_HOST]
    for helper in CAMBI_HOST_RESIDUAL:
        if helper.search(_code(host)):
            failures.append(f"{CAMBI_HOST}: host residual {helper.pattern} reintroduced")
    failures += _waits_outside(host, CAMBI_HOST, "collect_fex_cuda")
    failures += _readback_failures(host, CAMBI_HOST, "CambiCudaResults")
    if "vmaf_cambi_check_window_fits_lut(" not in host:
        failures.append(f"{CAMBI_HOST}: cambi.c's reciprocal-LUT window guard is not applied")
    kernels = sources[CAMBI_KERNELS]
    failures += _cambi_kernel_failures(kernels)
    return failures


def _cambi_kernel_failures(kernels: str) -> list[str]:
    failures: list[str] = []
    if re.search(r"\bdouble\b", kernels):
        failures.append(f"{CAMBI_KERNELS}: fp64 in the device top-K or c-values")
    # c_value_pixel(): (float)(w * p0 * pm) * reciprocal_lut[pm + p0], one
    # conversion and one uncontracted multiply by the uploaded table.
    cvalue = re.compile(
        r"__fmul_rn\(__int2float_rn\(__ldg\(c\.weights \+ d\) \* p0 \* pm\),\s*"
        r"__ldg\(c\.lut \+ pm \+ p0\)\)"
    )
    if not cvalue.search(kernels):
        failures.append(f"{CAMBI_KERNELS}: c-value is not (float)(w*p0*pm) * lut[pm + p0]")
    if "__float2ull_rz(__fmul_rn(value, CAMBI_FIXED_SCALE))" not in kernels:
        failures.append(f"{CAMBI_KERNELS}: top-K sum is not the exact 2^-24 fixed point")
    for name in ("cambi_cvals_kernel", "cambi_radix_scan_kernel", "cambi_topk_final_kernel"):
        if name not in kernels:
            failures.append(f"{CAMBI_KERNELS}: {name} missing")
    failures += _single_struct_kernels(kernels, CAMBI_KERNELS, r"CambiCuda\w+Args")
    return failures


def _speed_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in SPEED_HOSTS:
        code = _code(sources[name])
        for helper in SPEED_HOST_RESIDUAL:
            if helper.search(code):
                failures.append(f"{name}: host residual {helper.pattern} reintroduced")
        failures += _waits_outside(sources[name], name, "speed_cuda_pipeline_wait")
        pipeline = name == "speed_cuda_pipeline.c"
        failures += _readback_failures(
            sources[name], name, "SpeedGpuFrameResult" if pipeline else None
        )
        if re.search(r"\bcuLaunchKernel\s*\(", code) and not pipeline:
            failures.append(f"{name}: kernel launched outside speed_cuda_pipeline.c")
    kernels = sources[SPEED_KERNELS]
    if re.search(r"\bdouble\b", kernels):
        failures.append(f"{SPEED_KERNELS}: fp64 type appears in the device pipeline")
    # Correctly rounded fp32: division and square root only through the
    # round-to-nearest intrinsics; no fast or library approximations.
    if re.search(
        r"\b(?:sqrtf|rsqrtf|log2f|logf|expf|powf|__fdividef|__log2f|__powf|__expf)\s*\(",
        _code(kernels),
    ):
        failures.append(f"{SPEED_KERNELS}: approximate math replaces a round-to-nearest operation")
    for intrinsic in ("__fdiv_rn", "__fsqrt_rn", "__fadd_rn", "__fmul_rn"):
        if intrinsic not in kernels:
            failures.append(f"{SPEED_KERNELS}: {intrinsic} no longer spells the CPU rounding")
    if "return speed_log2_hard_case(" not in kernels:
        failures.append(f"{SPEED_KERNELS}: speed_log2 no longer applies the log2 hard cases")
    if "0x1.0c6f7ap-20f" not in kernels or "0x1.6bdb1ap-49f" not in kernels:
        failures.append(f"{SPEED_KERNELS}: EIGENVALUE_EPS no longer compared exactly")
    failures += _single_struct_kernels(kernels, SPEED_KERNELS, "SpeedCudaFrameArgs")
    if (
        "'speed_score' : vmaf_cuda_host_strict_fp_args + ['--fmad=false']"
        not in sources["meson.build"]
    ):
        failures.append("core/src/meson.build: speed_score fatbin is not built with --fmad=false")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return _cambi_failures(sources) + _speed_failures(sources)


def _hard_case_table(text: str) -> list[tuple[int, int]]:
    def values(name: str) -> list[int]:
        match = re.search(rf"#define {name}\b(.*?)\}}", text, re.S)
        return [int(token, 16) for token in re.findall(r"0x([0-9a-f]{8})u", match.group(1))]

    return list(zip(values("SPEED_LOG2_HARD_INPUTS"), values("SPEED_LOG2_HARD_OUTPUTS")))


def _f32(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def _is_nearest_log2(x_bits: int, y_bits: int) -> bool:
    """Whether float32 `y` is the float nearest log2(x), decided in 60 digits."""
    getcontext().prec = 60
    exact = Decimal(_f32(x_bits)).ln() / Decimal(2).ln()
    error = abs(Decimal(_f32(y_bits)) - exact)
    neighbours = (y_bits - 1, y_bits + 1)
    return all(error < abs(Decimal(_f32(other)) - exact) for other in neighbours)


def _hard_case_failures(text: str) -> list[str]:
    table = _hard_case_table(text)
    failures = [] if len(table) == 48 else [f"log2 hard cases: {len(table)} entries, expected 48"]
    inputs = [x for x, _ in table]
    if inputs != sorted(set(inputs)):
        failures.append("log2 hard cases: inputs not strictly ascending")
    for x_bits, y_bits in table:
        if x_bits & 0x7FFFFF not in (0x554996, 0x7FC006):
            failures.append(f"log2 hard cases: 0x{x_bits:08x} outside the two known mantissas")
        if not _is_nearest_log2(x_bits, y_bits):
            failures.append(f"log2 hard cases: 0x{y_bits:08x} is not log2(0x{x_bits:08x}) rounded")
    return failures


class CudaKernelSourceContractTest(unittest.TestCase):
    def test_live_sources_keep_the_device_resident_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_log2_hard_cases_are_correctly_rounded(self) -> None:
        self.assertEqual(_hard_case_failures(LOG2_HARD_CASES.read_text(encoding="utf-8")), [])

    def test_wrong_log2_hard_case_is_detected(self) -> None:
        # The device value of the first entry, one ulp off the correct one.
        text = LOG2_HARD_CASES.read_text(encoding="utf-8").replace("0xc1fa1b55u", "0xc1fa1b54u", 1)
        self.assertTrue(any("is not log2" in item for item in _hard_case_failures(text)))

    def test_dropped_log2_hard_cases_are_detected(self) -> None:
        sources = _sources()
        sources[SPEED_KERNELS] = sources[SPEED_KERNELS].replace(
            "return speed_log2_hard_case(__float_as_uint(x), rounded);", "return rounded;", 1
        )
        self.assertTrue(any("log2 hard cases" in item for item in _contract_failures(sources)))

    def test_cambi_host_c_values_residual_is_detected(self) -> None:
        # The pre-ADR-1379 cambi_submit_scale() ran the c-values on the host.
        sources = _sources()
        sources[CAMBI_HOST] += (
            "\nvmaf_cambi_calculate_c_values(&s->pics[0], &s->pics[1], s->buffers.c_values,"
            " NULL, 0, 0, NULL, 0, NULL, NULL, 0, 0, NULL, NULL);\n"
            "*score_out = vmaf_cambi_spatial_pooling(s->buffers.c_values, topk, w, h);\n"
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("vmaf_cambi_calculate_c_values" in item for item in failures))
        self.assertTrue(any("vmaf_cambi_spatial_pooling" in item for item in failures))

    def test_cambi_per_scale_sync_is_detected(self) -> None:
        # The pre-ADR-1379 cambi_filter_and_readback() waited on every scale.
        sources = _sources()
        sources[CAMBI_HOST] += "\nCHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(stream));\n"
        self.assertTrue(any("host wait" in item for item in _contract_failures(sources)))

    def test_cambi_per_scale_readback_is_detected(self) -> None:
        # The pre-ADR-1379 cambi_filter_and_readback() copied the image and the
        # mask of every scale back for the host c-values.
        sources = _sources()
        sources[CAMBI_HOST] += (
            "\nCHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->h_image,"
            " cambi_dptr(s, CAMBI_BUF_IMAGE), bytes, stream));\n"
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("2 device-to-host copies" in item for item in failures))

    def test_cambi_host_combine_of_c_values_is_detected(self) -> None:
        # Reading the c-value plane back to pool it on the host is a host combine.
        sources = _sources()
        sources[CAMBI_HOST] = sources[CAMBI_HOST].replace(
            "sizeof(CambiCudaResults), s->lc.str",
            "cambi_buffer_bytes(s, CAMBI_BUF_CVALS), s->lc.str",
            1,
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("not the CambiCudaResults block" in item for item in failures))

    def test_cambi_host_preprocessing_is_detected(self) -> None:
        sources = _sources()
        sources[CAMBI_HOST] += (
            "\nint err = vmaf_cuda_picture_download_async(dist_pic, dist_host, 0x1);\n"
            "err = vmaf_cambi_preprocessing(dist_host, &s->pics[0], w, h, bd);\n"
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("download_async" in item for item in failures))
        self.assertTrue(any("vmaf_cambi_preprocessing" in item for item in failures))

    def test_cambi_missing_window_guard_is_detected(self) -> None:
        sources = _sources()
        sources[CAMBI_HOST] = sources[CAMBI_HOST].replace(
            "vmaf_cambi_check_window_fits_lut(", "cambi_skip_window_guard("
        )
        self.assertTrue(any("window guard" in item for item in _contract_failures(sources)))

    def test_cambi_reciprocal_division_is_detected(self) -> None:
        # 42 of cambi.c's 4226 reciprocals differ from 1.0f / i by one ulp.
        sources = _sources()
        sources[CAMBI_KERNELS] = sources[CAMBI_KERNELS].replace(
            "__fmul_rn(__int2float_rn(__ldg(c.weights + d) * p0 * pm), __ldg(c.lut + pm + p0))",
            "(float)(__ldg(c.weights + d) * p0 * pm) / (float)(pm + p0)",
        )
        self.assertTrue(any("lut[pm + p0]" in item for item in _contract_failures(sources)))

    def test_cambi_double_top_k_is_detected(self) -> None:
        sources = _sources()
        sources[CAMBI_KERNELS] += "\nstatic __device__ double topk_sum;\n"
        self.assertTrue(any("fp64" in item for item in _contract_failures(sources)))

    def test_cambi_loose_kernel_arguments_are_detected(self) -> None:
        # ADR-1215: a surplus launch argument is silently ignored by the driver.
        sources = _sources()
        sources[CAMBI_KERNELS] = sources[CAMBI_KERNELS].replace(
            "cambi_decimate_kernel(const CambiCudaDecimateArgs a)",
            "cambi_decimate_kernel(const uint16_t *src, uint16_t *dst, unsigned out_width)",
            1,
        )
        self.assertTrue(
            any("cambi_decimate_kernel" in item for item in _contract_failures(sources))
        )

    def test_speed_host_linear_algebra_is_detected(self) -> None:
        # The pre-ADR-1380 run_cpu_linalg() solved the 25x25 system on the host.
        sources = _sources()
        sources["speed_chroma_cuda.c"] += (
            "\nspeed_internal_compute_eigenvalues(s->h_cov_mat, s->h_eigenvalues, 25, scratch);\n"
            "(void)speed_internal_qr_factorize(s->h_cov_mat, 25, s->h_Q, s->h_R, tmp);\n"
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("speed_internal_compute_eigenvalues" in item for item in failures))
        self.assertTrue(any("speed_internal_qr_factorize" in item for item in failures))

    def test_speed_host_filter_is_detected(self) -> None:
        # The pre-ADR-1380 sc_prepare_planes() downloaded and filtered on the host.
        sources = _sources()
        sources["speed_temporal_cuda.c"] += (
            "\nif (cu_f->cuMemcpyDtoH(raw_ref, (CUdeviceptr)ref_pic->data[0], bytes)) return -EIO;\n"
            "picture_copy(s->h_ref[0], s->float_stride, &host_ref, -128, bpc, 0);\n"
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("picture_copy" in item for item in failures))
        self.assertTrue(any("host wait" in item for item in failures))

    def test_speed_host_score_combine_is_detected(self) -> None:
        # The pre-ADR-1380 sc_run_score_kernel() read the per-block entropies
        # and variances back and sc_aggregate_score() combined them on the host.
        sources = _sources()
        sources["speed_chroma_cuda.c"] += (
            "\nCHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->h_ref_entropies,"
            " s->d_ref_entropies, ab, s->stream));\n"
        )
        failures = _contract_failures(sources)
        self.assertTrue(
            any("device-to-host copy outside the pipeline" in item for item in failures)
        )

    def test_speed_second_readback_is_detected(self) -> None:
        sources = _sources()
        sources["speed_cuda_pipeline.c"] += (
            "\nCHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(p->h_cov, speed_dptr(p, SPEED_BUF_COV),"
            " 625 * sizeof(float), p->lc.str));\n"
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("2 device-to-host copies" in item for item in failures))

    def test_speed_mid_frame_sync_is_detected(self) -> None:
        sources = _sources()
        sources[
            "speed_cuda_pipeline.c"
        ] += "\nCHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(stream));\n"
        self.assertTrue(any("host wait" in item for item in _contract_failures(sources)))

    def test_speed_fp64_is_detected(self) -> None:
        sources = _sources()
        sources[SPEED_KERNELS] = sources[SPEED_KERNELS].replace(
            "struct Ff {", "struct Wide { double sum; };\nstruct Ff {", 1
        )
        self.assertTrue(any("fp64 type" in item for item in _contract_failures(sources)))

    def test_speed_approximate_sqrt_is_detected(self) -> None:
        sources = _sources()
        sources[SPEED_KERNELS] = sources[SPEED_KERNELS].replace(
            "return __fsqrt_rn(x);", "return sqrtf(x);", 1
        )
        self.assertTrue(any("approximate math" in item for item in _contract_failures(sources)))

    def test_speed_contracting_build_is_detected(self) -> None:
        sources = _sources()
        sources["meson.build"] = sources["meson.build"].replace(
            "'speed_score' : vmaf_cuda_host_strict_fp_args + ['--fmad=false'],", "", 1
        )
        self.assertTrue(any("--fmad=false" in item for item in _contract_failures(sources)))

    def test_speed_kernel_in_extractor_tu_is_detected(self) -> None:
        sources = _sources()
        sources["speed_temporal_cuda.c"] += (
            "\nCHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_score, 1, 1, 1, 256, 1, 1, 0,"
            " stream, args, NULL));\n"
        )
        self.assertTrue(any("launched outside" in item for item in _contract_failures(sources)))


if __name__ == "__main__":
    unittest.main()
