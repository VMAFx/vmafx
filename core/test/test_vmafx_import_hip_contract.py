#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the VMAFx HIP lane at the source level, device-free (ADR-2092).

No AMD device runs in CI, so this test reads the sources and checks the
shapes the lane's device tests (test_vmafx_import_hip*, on a HIP device)
depend on:

- every HIP twin that reads picture planes on the host (psnr_hvs,
  ssimulacra2, float_ms_ssim's level 0) asks whether a picture is a device
  picture first and reads a device picture on the device;
- device pictures are copied on their library stream, and both the reading
  twin's stream and the null stream wait for the copies (integer_adm_hip,
  psnr_hip and float_vif_hip read on the null stream);
- an import submits its library-stream work before the frame is bound (the
  gfx1036 let later copies overtake an unsubmitted array readout);
- a GL import is exported as dma-bufs (ADR-2132), never read through the
  runtime's GL interop (unreadable on ROCm 10.1, crashing on ROCm 7.2 after a
  failed first call): the GL sync is checked before anything is exported, the
  context's GPU is checked against the device's before any texture is
  exported, a texture the driver exports tiled is copied on the GPU only with
  VMAFX_IMPORT_ALLOW_COPY and the producer's pending writes are waited for;
- a dma-buf is imported with its own size, and a larger size from the
  producer is refused (the runtime does not check it);
- a SYNC_FILE release fence is refused, not faked.

Every check has a planted-regression case that must be detected.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "core" / "src"

PICTURE = "hip/picture_hip.c"
FRAME = "hip/import_frame.c"
GL = "hip/import_gl.c"
EGL = "vmafx/egl_export.c"
DMABUF = "hip/import_dmabuf.c"
FENCE = "hip/import_fence.c"
PSNR_HVS = "feature/hip/integer_psnr_hvs_hip.c"
SS2 = "feature/hip/ssimulacra2_hip.c"
MS_SSIM = "feature/hip/integer_ms_ssim_hip.c"
SOURCES = (PICTURE, FRAME, GL, EGL, DMABUF, FENCE, PSNR_HVS, SS2, MS_SSIM)


def _sources() -> dict[str, str]:
    return {name: (SRC / name).read_text(encoding="utf-8") for name in SOURCES}


def _function_body(source: str, name: str) -> str:
    """The definition of C function `name`, signature to closing brace."""
    match = re.search(r"^[A-Za-z][\w \*]*\b" + re.escape(name) + r"\([^;{]*\)\s*\{", source, re.M)
    if not match:
        return ""
    depth = 0
    for at in range(match.end() - 1, len(source)):
        depth += {"{": 1, "}": -1}.get(source[at], 0)
        if depth == 0:
            return source[match.start() : at + 1]
    return ""


def _in_order(text: str, *needles: str) -> bool:
    at = 0
    for needle in needles:
        found = text.find(needle, at)
        if found < 0:
            return False
        at = found + len(needle)
    return True


def _twin_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    take = _function_body(src[PSNR_HVS], "psnr_hvs_take_pictures")
    if not _in_order(
        take, "vmaf_hip_picture_device_stream(", "psnr_hvs_copy_device(", "psnr_hvs_stage_plane("
    ):
        failures.append(f"{PSNR_HVS}: a device picture is staged through the host")
    take = _function_body(src[SS2], "ss2h_take_pictures")
    if not _in_order(
        take, "vmaf_hip_picture_device_stream(", "ss2h_copy_device(", "ss2h_stage_plane("
    ):
        failures.append(f"{SS2}: a device picture is staged through the host")
    upload = _function_body(src[MS_SSIM], "ms_ssim_hip_upload_plane")
    if not _in_order(
        upload, "vmaf_hip_picture_device_stream(", "ms_ssim_hip_convert_device(", "picture_copy("
    ):
        failures.append(f"{MS_SSIM}: a device picture's level 0 is built on the host")
    return failures


def _picture_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    text = src[PICTURE]
    for name in ("vmaf_hip_picture_upload", "vmaf_hip_picture_upload_staged"):
        body = _function_body(text, name)
        if not _in_order(body, "hip_pic_common_library(", "hip_pic_device_upload("):
            failures.append(f"{PICTURE}: {name} reads a device picture as host memory")
    wait = _function_body(text, "vmaf_hip_stream_wait_library")
    if not _in_order(
        wait,
        "hipEventRecord(",
        "hipStreamWaitEvent(vmaf_hip_stream_of(stream)",
        "hipStreamWaitEvent(vmaf_hip_stream_of(0u)",
    ):
        failures.append(f"{PICTURE}: a reader's stream or the null stream runs ahead of the copies")
    enqueue = _function_body(text, "vmaf_hip_picture_copy_enqueue")
    if "hipMemcpyDeviceToDevice" not in enqueue or "hipMemcpyDeviceToHost" in text:
        failures.append(f"{PICTURE}: a device picture's plane leaves the device")
    return failures


def _import_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    bind = _function_body(src[FRAME], "bind_hip_frame")
    if not _in_order(bind, "enqueue_import(", "hipStreamQuery(", "bind_picture("):
        failures.append(f"{FRAME}: the import's work is left unsubmitted")
    gl_import = _function_body(src[FRAME], "import_gl")
    if not _in_order(
        gl_import, "vmafx_gl_sync_acquire(", "vmafx_hip_gl_export(", "bind_hip_frame("
    ):
        failures.append(f"{FRAME}: a GL texture is exported before its GL sync is checked")
    if "hipGraphics" in src[GL] + src[FRAME]:
        failures.append(f"{GL}: a GL texture is read through the runtime's GL interop")
    planes = _function_body(src[EGL], "vmafx_egl_export_planes")
    if not _in_order(planes, "open_display(", "export_one("):
        failures.append(f"{EGL}: a texture is exported before the context's GPU is checked")
    opened = _function_body(src[EGL], "open_display")
    if "strcasecmp(pci, device_pci)" not in opened:
        failures.append(f"{EGL}: the context's GPU is not checked against the device's")
    one = _function_body(src[EGL], "export_one")
    if not _in_order(one, "if (!allow_copy ||", "linearise("):
        failures.append(f"{EGL}: a tiled texture is copied without VMAFX_IMPORT_ALLOW_COPY")
    if "writers_done(" not in planes:
        failures.append(f"{EGL}: the producer's pending writes are not waited for")
    one = _function_body(src[DMABUF], "import_one")
    if ".size = dmabuf_size(own)" not in one:
        failures.append(f"{DMABUF}: a dma-buf is imported with a size the runtime does not check")
    extent = _function_body(src[DMABUF], "check_extent")
    if "p->size > actual" not in extent:
        failures.append(f"{DMABUF}: a size past the dma-buf is accepted")
    refused = _function_body(src[FENCE], "release_kind_refused")
    if "VMAFX_FENCE_SYNC_FILE" not in refused or "VMAFX_E_NOTSUP" not in refused:
        failures.append(f"{FENCE}: a SYNC_FILE release fence is not refused")
    return failures


def _failures(src: dict[str, str]) -> list[str]:
    return _twin_failures(src) + _picture_failures(src) + _import_failures(src)


def _replace(src: dict[str, str], name: str, old: str, new: str) -> dict[str, str]:
    if old not in src[name]:
        raise AssertionError(f"planted regression anchor missing in {name}: {old!r}")
    out = dict(src)
    out[name] = src[name].replace(old, new, 1)
    return out


class HipImportContractTest(unittest.TestCase):
    def assert_detected(self, src: dict[str, str], needle: str) -> None:
        failures = _failures(src)
        self.assertTrue(any(needle in f for f in failures), failures)

    def test_sources_hold_the_contract(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_staged_device_picture_is_detected(self) -> None:
        src = _replace(
            _sources(),
            PSNR_HVS,
            "    s->device_input = vmaf_hip_picture_device_stream(ref) != 0u;",
            "    s->device_input = false;",
        )
        src = _replace(src, PSNR_HVS, "        return psnr_hvs_copy_device(s, ref, dist);\n", "")
        self.assert_detected(src, "staged through the host")

    def test_staged_ssimulacra2_device_picture_is_detected(self) -> None:
        src = _replace(
            _sources(),
            SS2,
            "    if (s->device_input)\n        return ss2h_copy_device(s, pics);\n",
            "",
        )
        self.assert_detected(src, f"{SS2}: a device picture is staged through the host")

    def test_host_level0_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_SSIM,
            "    if (vmaf_hip_picture_device_stream(pic) != 0u)\n"
            "        return ms_ssim_hip_convert_device(s, str, pic, plane, d_dst);\n",
            "",
        )
        self.assert_detected(src, "level 0 is built on the host")

    def test_upload_of_a_device_picture_as_host_is_detected(self) -> None:
        src = _replace(
            _sources(),
            PICTURE,
            "    if (library != 0u)\n"
            "        return hip_pic_device_upload(planes, n_planes, library, stream);\n",
            "",
        )
        self.assert_detected(src, "reads a device picture as host memory")

    def test_null_stream_without_the_wait_is_detected(self) -> None:
        src = _replace(
            _sources(),
            PICTURE,
            "        rc = hipStreamWaitEvent(vmaf_hip_stream_of(0u), read, 0u);",
            "        rc = hipSuccess;",
        )
        self.assert_detected(src, "runs ahead of the copies")

    def test_unsubmitted_import_is_detected(self) -> None:
        src = _replace(_sources(), FRAME, "    (void)hipStreamQuery(hf->dev->str);\n", "")
        self.assert_detected(src, "left unsubmitted")

    def test_gl_export_before_the_sync_check_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FRAME,
            '    VmafxStatus status = vmafx_gl_sync_acquire(report, &desc->acquire, "hip");',
            "    VmafxStatus status = VMAFX_OK;",
        )
        self.assert_detected(src, "before its GL sync is checked")

    def test_runtime_gl_interop_is_detected(self) -> None:
        src = _replace(
            _sources(),
            GL,
            "/* NOLINTEND(modernize-use-nullptr) */",
            "static void bad(void) { hipGraphicsMapResources(0, 0, 0); }",
        )
        self.assert_detected(src, "through the runtime's GL interop")

    def test_export_before_the_gpu_check_is_detected(self) -> None:
        src = _replace(
            _sources(),
            EGL,
            "    VmafxStatus status = open_display(report, backend, device_pci, allow_copy, &dpy, &ctx, node);",
            "    VmafxStatus status = VMAFX_OK;",
        )
        self.assert_detected(src, "before the context's GPU is checked")

    def test_unchecked_gpu_is_detected(self) -> None:
        src = _replace(_sources(), EGL, "strcasecmp(pci, device_pci)", "strcasecmp(pci, pci)")
        self.assert_detected(src, "GPU is not checked against the device's")

    def test_copy_without_allow_copy_is_detected(self) -> None:
        src = _replace(
            _sources(), EGL, "if (!allow_copy || !gbm_dev || !gl_ready())", "if (!gbm_dev)"
        )
        self.assert_detected(src, "without VMAFX_IMPORT_ALLOW_COPY")

    def test_unwaited_writers_are_detected(self) -> None:
        src = _replace(
            _sources(), EGL, "const int w = writers_done(out[i].fd);", "const int w = 0;"
        )
        self.assert_detected(src, "pending writes are not waited for")

    def test_unchecked_dmabuf_size_is_detected(self) -> None:
        src = _replace(_sources(), DMABUF, ".size = dmabuf_size(own)", ".size = 0u")
        self.assert_detected(src, "size the runtime does not check")
        src = _replace(_sources(), DMABUF, "    if (p->size > actual) {", "    if (false) {")
        self.assert_detected(src, "past the dma-buf is accepted")

    def test_faked_sync_file_release_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FENCE,
            "    if (kind == VMAFX_FENCE_SYNC_FILE) {",
            "    if (kind == VMAFX_FENCE_NONE) {",
        )
        self.assert_detected(src, "SYNC_FILE release fence is not refused")


if __name__ == "__main__":
    unittest.main()
