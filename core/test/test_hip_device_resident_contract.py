#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the device-resident HIP CAMBI and SpEED design at the source level.

ADR-1378 (cambi_hip) and ADR-1384 (speed_chroma_hip, speed_temporal_hip) run
every stage of cambi.c and speed.c on the device, with one upload and one
result readback per frame and one host wait, in collect(). No AMD device runs
in CI, so this test reads the sources and checks the load-bearing shapes: no
host stage of the CPU extractors is called, submit() stages its planes without
waiting and reads back once, collect() holds the frame's only wait and
combines only through the CPU's shared helpers, the device code is fp64-free
and uses no HIP intrinsic that rounds or contracts differently from the CPU,
and the SpEED kernel build keeps its exact-arithmetic flags. Every check has a
planted-regression case that reintroduces the old shape and must be detected.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# One definition of "a host wait" and of a C function body for every HIP
# source contract (ADR-1377's test owns them).
from test_hip_kernel_source_contract import HOST_WAIT, _function_body

ROOT = Path(__file__).resolve().parents[2]
HIP_FEATURE = ROOT / "core" / "src" / "feature" / "hip"
MESON = ROOT / "core" / "src" / "meson.build"

CAMBI_HOST = "integer_cambi_hip.c"
CAMBI_KERNEL = "integer_cambi/cambi_score.hip"
CAMBI_DEVICE = "integer_cambi/cambi_hip_device.h"
SPEED_PIPELINE = "speed_hip_pipeline.c"
SPEED_KERNEL = "speed/speed_pipeline.hip"
SPEED_DEVICE = "speed/speed_hip_device.h"
SPEED_TWINS = ("speed_chroma_hip.c", "speed_temporal_hip.c")
DEVICE_CODE = (CAMBI_KERNEL, CAMBI_DEVICE, SPEED_KERNEL, SPEED_DEVICE)

# Calls, not mentions: the sources cite these stages in comments.
CAMBI_HOST_STAGES = tuple(
    re.compile(rf"\b{name}\(")
    for name in (
        "vmaf_cambi_preprocessing",
        "vmaf_cambi_get_spatial_mask",
        "vmaf_cambi_decimate",
        "vmaf_cambi_filter_mode",
        "vmaf_cambi_calculate_c_values",
        "vmaf_cambi_spatial_pooling",
    )
)
SPEED_HOST_STAGES = tuple(
    re.compile(rf"\b{name}\(")
    for name in (
        "picture_copy",
        "speed_internal_filter_and_downscale",
        "speed_internal_compute_means",
        "speed_internal_compute_eigenvalues",
        "speed_internal_is_matrix_regular",
        "speed_internal_qr_factorize",
        "speed_internal_qt_multiply",
    )
)
# Per-frame enqueue paths: none may wait or copy synchronously.
CAMBI_FRAME_FNS = (
    "submit_fex_hip",
    "cambi_hip_enqueue_frame",
    "cambi_hip_enqueue_scale",
    "cambi_hip_enqueue_pool",
    "cambi_hip_launch",
)
SPEED_FRAME_FNS = (
    "speed_hip_pipeline_upload",
    "speed_hip_pipeline_submit",
    "speed_hip_enqueue_chain",
    "speed_hip_launch",
)
DEVICE_TO_HOST = re.compile(r"\bhipMemcpy\w*\([^;]*hipMemcpyDeviceToHost", re.S)
# Without OCML_BASIC_ROUNDED_OPERATIONS __fsqrt_rn() is the native
# approximation and __fmul_rn() / __fadd_rn() / __fdiv_rn() are the plain
# operators; the fast math builtins round differently from the CPU.
INEXACT_INTRINSICS = re.compile(
    r"\b(?:__fsqrt_rn|__fdiv_rn|__fmul_rn|__fadd_rn|__fsub_rn|__fmaf_rn|__fdividef|"
    r"__frsqrt_rn|__log2f|__logf|__expf|__powf|__saturatef|native_\w+)\s*\("
)


def _sources() -> dict[str, str]:
    names = (CAMBI_HOST, SPEED_PIPELINE, *SPEED_TWINS, *DEVICE_CODE)
    sources = {name: (HIP_FEATURE / name).read_text(encoding="utf-8") for name in names}
    sources["meson.build"] = MESON.read_text(encoding="utf-8")
    return sources


def _code(text: str) -> str:
    """`text` without C comments (the sources name the banned calls in prose)."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def _body(text: str, name: str) -> str:
    return _code(_function_body(text, name))


def _host_stage_failures(name: str, text: str, stages: tuple[re.Pattern[str], ...]) -> list[str]:
    """A CPU-extractor stage called on the host, or a sync outside the lifecycle."""
    code = _code(text)
    failures = [f"{name}: host stage {s.pattern} is back" for s in stages if s.search(code)]
    if "hipStreamSynchronize" in code or "hipDeviceSynchronize" in code:
        failures.append(f"{name}: a stream or device sync outside the lifecycle")
    return failures


def _frame_path_failures(name: str, text: str, fns: tuple[str, ...], upload_fn: str) -> list[str]:
    """The per-frame enqueue path: no host wait, a staged upload, one readback."""
    failures: list[str] = []
    for fn in fns:
        body = _body(text, fn)
        if not body:
            failures.append(f"{name}: {fn}() not found")
        elif HOST_WAIT.search(body):
            failures.append(f"{name}: {fn}() waits on the host mid-frame")
    if "vmaf_hip_picture_upload_staged(" not in _body(text, upload_fn):
        failures.append(f"{name}: the frame upload skips the pinned staging")
    readbacks = sum(len(DEVICE_TO_HOST.findall(_body(text, fn))) for fn in fns)
    if readbacks != 1:
        failures.append(f"{name}: {readbacks} device-to-host copies per frame, not one")
    return failures


def _cambi_collect_failures(host: str) -> list[str]:
    failures: list[str] = []
    collect = _body(host, "collect_fex_hip")
    if collect.count("vmaf_hip_kernel_collect_wait(") != 1:
        failures.append(f"{CAMBI_HOST}: collect() no longer holds the frame's one wait")
    for helper in ("vmaf_cambi_fixed_topk_mean(", "vmaf_cambi_weight_scores_per_scale("):
        if helper not in collect:
            failures.append(f"{CAMBI_HOST}: collect() combines without {helper})")
    if re.search(r"\bldexp\s*\(|18446744073709551616", _code(host)):
        failures.append(f"{CAMBI_HOST}: a local copy of the top-K mean is back")
    return failures


def _cambi_failures(src: dict[str, str]) -> list[str]:
    host = src[CAMBI_HOST]
    failures = _host_stage_failures(CAMBI_HOST, host, CAMBI_HOST_STAGES)
    failures += _frame_path_failures(CAMBI_HOST, host, CAMBI_FRAME_FNS, "cambi_hip_enqueue_frame")
    failures += _cambi_collect_failures(host)
    init = _body(host, "init_fex_hip")
    guard = init.find("cambi_hip_configure(")
    if guard < 0 or guard > init.find("cambi_hip_setup_device("):
        failures.append(f"{CAMBI_HOST}: init() touches the device before the window guard")
    if "vmaf_cambi_check_window_fits_lut(" not in _body(host, "cambi_hip_check_window"):
        failures.append(f"{CAMBI_HOST}: the window guard is not cambi.c's")
    return failures


def _speed_twin_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in SPEED_TWINS:
        code = _code(src[name])
        if HOST_WAIT.search(code) or "hipModuleLaunchKernel" in code:
            failures.append(f"{name}: waits or launches outside {SPEED_PIPELINE}")
        if "speed_internal_gpu_configure(" not in code:
            failures.append(f"{name}: init() skips the shared speed_internal_gpu_configure()")
    return failures


def _speed_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in (SPEED_PIPELINE, *SPEED_TWINS):
        failures += _host_stage_failures(name, src[name], SPEED_HOST_STAGES)
    failures += _speed_twin_failures(src)
    # The runtime half, not the -ENOSYS stubs of a build without hipcc.
    pipeline = src[SPEED_PIPELINE].split("#else /* HAVE_HIPCC */")[-1]
    failures += _frame_path_failures(
        SPEED_PIPELINE, pipeline, SPEED_FRAME_FNS, "speed_hip_pipeline_upload"
    )
    if "vmaf_hip_kernel_collect_wait(" not in _body(pipeline, "speed_hip_pipeline_wait"):
        failures.append(f"{SPEED_PIPELINE}: the frame's one wait moved out of collect")
    flags = re.search(r"'speed_pipeline'\s*:\s*\[([^\]]*)\]", src["meson.build"])
    wanted = ("'-ffp-contract=off'", "'-fhip-fp32-correctly-rounded-divide-sqrt'")
    if not flags or any(flag not in flags.group(1) for flag in wanted):
        failures.append("meson.build: the speed_pipeline kernel lost its exact-arithmetic flags")
    seam = r"#if defined\(SPEED_HD_HOST_LIBM_LOG2\) && !defined\(__HIP_DEVICE_COMPILE__\)"
    if not re.search(seam, _code(src[SPEED_DEVICE])):
        failures.append(f"{SPEED_DEVICE}: the host log2f test seam can reach device code")
    return failures


def _device_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in DEVICE_CODE:
        code = _code(src[name])
        if re.search(r"\bdouble\b", code):
            failures.append(f"{name}: fp64 in device code")
        if INEXACT_INTRINSICS.search(code):
            failures.append(f"{name}: an inexact HIP math intrinsic in device code")
    return failures


# Each twin's init(): the scaffold -ENOSYS comes before its host configure.
SCAFFOLD_FIRST = (
    (CAMBI_HOST, "init_fex_hip", "cambi_hip_configure("),
    ("speed_chroma_hip.c", "init_chroma_hip", "sc_configure("),
    ("speed_temporal_hip.c", "init_temporal_hip", "st_configure("),
)


def _scaffold_failures(src: dict[str, str]) -> list[str]:
    """A build without hipcc reports -ENOSYS and nothing else (ADR-1264)."""
    failures: list[str] = []
    for name, fn, configure in SCAFFOLD_FIRST:
        init = _body(src[name], fn)
        scaffold = init.find("return -ENOSYS;")
        if scaffold < 0 or not scaffold < init.find(configure):
            failures.append(f"{name}: {fn}() must return -ENOSYS first without HIPCC")
    return failures


def _failures(src: dict[str, str]) -> list[str]:
    return (
        _cambi_failures(src)
        + _speed_failures(src)
        + _device_failures(src)
        + _scaffold_failures(src)
    )


def _replace(src: dict[str, str], name: str, old: str, new: str) -> dict[str, str]:
    if old not in src[name]:
        raise AssertionError(f"planted regression anchor missing in {name}: {old!r}")
    out = dict(src)
    out[name] = src[name].replace(old, new, 1)
    return out


class HipDeviceResidentContractTest(unittest.TestCase):
    def assert_detected(self, src: dict[str, str], needle: str) -> None:
        failures = _failures(src)
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_live_sources_keep_the_contract(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    # ---- cambi_hip ----

    def test_host_cambi_stage_is_detected(self) -> None:
        src = _replace(
            _sources(),
            CAMBI_HOST,
            "    int err = cambi_hip_enqueue_frame(s, dist_pic);",
            "    (void)vmaf_cambi_preprocessing(dist_pic, NULL, 0, 0, 8);\n"
            "    int err = cambi_hip_enqueue_frame(s, dist_pic);",
        )
        self.assert_detected(src, "host stage")

    def test_per_scale_readback_sync_is_detected(self) -> None:
        src = _replace(
            _sources(),
            CAMBI_HOST,
            "    if (!err)\n        err = cambi_hip_enqueue_pool(s, scale);",
            "    if (!err)\n        err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);\n"
            "    if (!err)\n        err = cambi_hip_enqueue_pool(s, scale);",
        )
        self.assert_detected(src, "waits on the host mid-frame")

    def test_waiting_cambi_upload_is_detected(self) -> None:
        src = _replace(
            _sources(),
            CAMBI_HOST,
            "vmaf_hip_picture_upload_staged(&plane, 1u, s->h_staging, s->src_bytes, s->lc.str)",
            "vmaf_hip_picture_upload(&plane, 1u, s->lc.str)",
        )
        self.assert_detected(src, "skips the pinned staging")

    def test_second_cambi_readback_is_detected(self) -> None:
        src = _replace(
            _sources(),
            CAMBI_HOST,
            "    if (!err)\n        err = cambi_hip_enqueue_pool(s, scale);",
            "    if (!err)\n        err = vmaf_hip_rc_to_errno(hipMemcpyAsync(s->h_results, "
            "s->params.cvals, 4u, hipMemcpyDeviceToHost, 0));\n"
            "    if (!err)\n        err = cambi_hip_enqueue_pool(s, scale);",
        )
        self.assert_detected(src, "device-to-host copies per frame")

    def test_local_topk_mean_is_detected(self) -> None:
        src = _replace(
            _sources(),
            CAMBI_HOST,
            "        scores[scale] = vmaf_cambi_fixed_topk_mean(r->sum_hi[scale], r->sum_lo[scale],\n"
            "                                                   s->params.scale[scale].topk);",
            "        scores[scale] = ldexp((double)r->sum_lo[scale], -24) /\n"
            "                        (double)s->params.scale[scale].topk;",
        )
        self.assert_detected(src, "without vmaf_cambi_fixed_topk_mean")

    def test_device_before_window_guard_is_detected(self) -> None:
        src = _replace(
            _sources(),
            CAMBI_HOST,
            "    int err = cambi_hip_configure(s, bpc, w, h);\n    if (!err)\n",
            "    int err = 0;\n    if (!err)\n",
        )
        self.assert_detected(src, "before the window guard")

    def test_cambi_scaffold_checks_first_is_detected(self) -> None:
        scaffold = (
            "#ifndef HAVE_HIPCC\n    /* Scaffold posture: -ENOSYS and nothing else (ADR-1264). */"
        )
        src = _replace(
            _sources(),
            CAMBI_HOST,
            scaffold,
            "    (void)cambi_hip_configure(fex->priv, bpc, w, h);\n" + scaffold,
        )
        self.assert_detected(src, "init_fex_hip() must return -ENOSYS first")

    def test_speed_scaffold_checks_first_is_detected(self) -> None:
        scaffold = (
            "#ifndef HAVE_HIPCC\n    /* Scaffold posture: -ENOSYS and nothing else (ADR-1264). */"
        )
        src = _replace(
            _sources(),
            "speed_chroma_hip.c",
            scaffold,
            "    (void)sc_configure(fex->priv, pix_fmt, bpc, w, h, NULL);\n" + scaffold,
        )
        self.assert_detected(src, "init_chroma_hip() must return -ENOSYS first")

    def test_fp64_in_cambi_device_code_is_detected(self) -> None:
        src = _replace(
            _sources(),
            CAMBI_DEVICE,
            "    return (uint64_t)(value * CAMBI_HIP_FIXED_SCALE);",
            "    return (uint64_t)((double)value * 16777216.0);",
        )
        self.assert_detected(src, "fp64 in device code")

    # ---- speed_chroma_hip / speed_temporal_hip ----

    def test_host_linear_algebra_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "speed_chroma_hip.c",
            "    const int err = speed_hip_pipeline_collect(s->pipeline, &result);",
            "    (void)speed_internal_compute_eigenvalues(NULL, NULL, 0);\n"
            "    const int err = speed_hip_pipeline_collect(s->pipeline, &result);",
        )
        self.assert_detected(src, "host stage")

    def test_host_filter_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "speed_temporal_hip.c",
            "    const uint32_t set = index % ST_SLOTS;",
            "    const uint32_t set = index % ST_SLOTS;\n"
            "    picture_copy(NULL, 0, ref_pic, -128, 8, 0);",
        )
        self.assert_detected(src, "host stage")

    def test_covariance_readback_sync_is_detected(self) -> None:
        src = _replace(
            _sources(),
            SPEED_PIPELINE,
            "    if (!err)\n        err = speed_hip_launch(p, SPEED_K_LINALG,",
            "    if (!err)\n        err = vmaf_hip_rc_to_errno(hipMemcpy(NULL, p->params.cov, 4u, "
            "hipMemcpyDeviceToHost));\n"
            "    if (!err)\n        err = speed_hip_launch(p, SPEED_K_LINALG,",
        )
        self.assert_detected(src, "waits on the host mid-frame")

    def test_waiting_speed_upload_is_detected(self) -> None:
        src = _replace(
            _sources(),
            SPEED_PIPELINE,
            "    return vmaf_hip_picture_upload_staged(uploads, count, p->h_staging, "
            "p->staging_bytes,\n                                          p->lc.str);",
            "    return vmaf_hip_picture_upload(uploads, count, p->lc.str);",
        )
        self.assert_detected(src, "skips the pinned staging")

    def test_twin_side_sync_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "speed_chroma_hip.c",
            "    if (!err)\n        err = speed_hip_pipeline_submit(s->pipeline, 0u);",
            "    if (!err)\n        err = speed_hip_pipeline_wait(s->pipeline);\n"
            "    (void)hipStreamSynchronize(NULL);\n"
            "    if (!err)\n        err = speed_hip_pipeline_submit(s->pipeline, 0u);",
        )
        self.assert_detected(src, "sync outside the lifecycle")

    def test_private_configure_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "speed_temporal_hip.c",
            "speed_internal_gpu_configure(&dim, &opt, bpc, &config->shared)",
            "speed_hip_local_configure(&dim, &opt, bpc, &config->shared)",
        )
        self.assert_detected(src, "shared speed_internal_gpu_configure")

    def test_contracting_speed_build_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "meson.build",
            "'speed_pipeline' : ['-ffp-contract=off', '-fhip-fp32-correctly-rounded-divide-sqrt']",
            "'speed_pipeline' : ['-fhip-fp32-correctly-rounded-divide-sqrt']",
        )
        self.assert_detected(src, "exact-arithmetic flags")

    def test_native_sqrt_is_detected(self) -> None:
        src = _replace(
            _sources(),
            SPEED_DEVICE,
            "    return sqrtf(xx + yy);",
            "    return __fsqrt_rn(xx + yy);",
        )
        self.assert_detected(src, "inexact HIP math intrinsic")

    def test_device_libm_log2_is_detected(self) -> None:
        src = _replace(
            _sources(),
            SPEED_DEVICE,
            "#if defined(SPEED_HD_HOST_LIBM_LOG2) && !defined(__HIP_DEVICE_COMPILE__)",
            "#if defined(SPEED_HD_HOST_LIBM_LOG2)",
        )
        self.assert_detected(src, "log2f test seam")

    def test_fp64_in_speed_kernel_is_detected(self) -> None:
        src = _replace(
            _sources(),
            SPEED_DEVICE,
            "    const float count = (float)(g->sub_w * g->sub_h);\n    return sum / count;",
            "    const double count = (double)(g->sub_w * g->sub_h);\n"
            "    return (float)(sum / count);",
        )
        self.assert_detected(src, "fp64 in device code")


if __name__ == "__main__":
    unittest.main()
