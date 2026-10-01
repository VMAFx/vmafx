#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the HIP shared frame at the source level, device-free.

ADR-1408: a VmafContext uploads each plane of a frame once and every HIP twin
reads that copy (core/src/hip/shared_frame.c). No AMD device runs in CI, so
this test reads the sources and checks the load-bearing shapes:

- an adopted twin gets its planes from the shared frame and closes its plane
  source; it does not upload a picture plane on its own;
- vmaf_read_pictures() announces the frame before the twins are dispatched
  and ends it after the last one, and vmaf_close() destroys the shared frame
  after the extractors are closed;
- the shared upload is the waiting one (the pageable-upload race,
  T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18), a slot is fenced before it is
  written, a twin lets go of its slot before it asks again, and nothing is
  served once the frame has ended;
- an upload takes along the planes the twins asked for in the frame before,
  so a frame costs one host wait, and that expectation lasts one frame.

Every check has a planted-regression case that must be detected.
test_hip_shared_frame (device-free, behaviour) and test_hip_upload_race (on
the device) are the other two halves.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "core" / "src"
HIP_FEATURE = SRC / "feature" / "hip"

SHARED = "hip/shared_frame.c"
LIBVMAF = "libvmaf.c"
FEX = "feature/feature_extractor.cpp"

# Twins that read whole picture planes and share them. A twin that packs or
# converts on the host (ms_ssim, cambi, SpEED) or that an open change still
# owns (psnr_hvs, ssimulacra2) is tracked in docs/state.md
# (T-HIP-SHARED-FRAME-REMAINING-TWINS-2026-10-01) and joins this tuple when it
# is adopted.
ADOPTED = (
    "ciede_hip.c",
    "float_adm_hip.c",
    "float_moment_hip.c",
    "float_motion_hip.c",
    "float_psnr_hip.c",
    "float_ssim_hip.c",
    "float_vif_hip.c",
    "integer_adm_hip.c",
    "integer_motion_hip.c",
    "integer_motion_v2_hip.c",
    "integer_psnr_hip.c",
    "integer_ssim_hip.c",
    "integer_vif_hip.c",
)

# A picture plane reaching the device without the shared frame.
OWN_UPLOAD = re.compile(
    r"\b(?:vmaf_hip_picture_upload|vmaf_hip_picture_upload_staged|hipMemcpy2DAsync|"
    r"hipMemcpy2D)\s*\("
)
ACQUIRE = re.compile(r"\bvmaf_hip_plane_source_acquire(?:_luma)?\s*\(")
# Any copy or wait other than the waiting upload.
RAW_COPY = re.compile(r"\bhipMemcpy\w*\s*\(")


def _sources() -> dict[str, str]:
    src = {name: (HIP_FEATURE / name).read_text(encoding="utf-8") for name in ADOPTED}
    for name in (SHARED, LIBVMAF, FEX):
        src[name] = (SRC / name).read_text(encoding="utf-8")
    return src


def _function_body(source: str, name: str) -> str:
    """The definition of C function `name`, signature to closing brace."""
    match = re.search(
        rf"^(?:static )?[A-Za-z_][\w \*]*\b{name}\((?:[^;{{]|\n)*?\)\s*\{{.*?^}}$",
        source,
        re.S | re.M,
    )
    return match.group(0) if match else ""


def _in_order(body: str, *needles: str) -> bool:
    """Every needle occurs in `body`, each after the one before it."""
    at = 0
    for needle in needles:
        at = body.find(needle, at)
        if at < 0:
            return False
        at += len(needle)
    return True


def _twin_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in ADOPTED:
        text = src[name]
        if OWN_UPLOAD.search(text):
            failures.append(f"{name}: uploads a picture plane without the shared frame")
        if not ACQUIRE.search(text) or "fex->hip_frame" not in text:
            failures.append(f"{name}: does not ask the context's shared frame for its planes")
        if "vmaf_hip_plane_source_close(" not in text:
            failures.append(f"{name}: never closes its plane source")
    return failures


def _framework_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    loop = _function_body(src[LIBVMAF], "read_pictures_extractor_loop")
    if not _in_order(
        loop,
        "read_pictures_hip_frame_begin(",
        "read_pictures_dispatch_extractors(",
        "vmaf_hip_shared_frame_end(vmaf->hip.frame)",
    ):
        failures.append(f"{LIBVMAF}: the frame is not announced before and ended after dispatch")
    begin = _function_body(src[LIBVMAF], "read_pictures_hip_frame_begin")
    if "vmaf_hip_shared_frame_begin(vmaf->hip.frame," not in begin:
        failures.append(f"{LIBVMAF}: the dispatch loop no longer announces the frame's pictures")
    bind = _function_body(src[LIBVMAF], "set_fex_hip_frame")
    if "fex_ctx->fex->hip_frame = vmaf->hip.frame" not in bind:
        failures.append(f"{LIBVMAF}: a HIP twin is not handed the context's shared frame")
    backends = _function_body(src[LIBVMAF], "vmaf_close_backends")
    owners = _function_body(src[LIBVMAF], "vmaf_commit_remaining_owners")
    prepare = _function_body(src[LIBVMAF], "vmaf_prepare_close")
    if (
        "vmaf_hip_shared_frame_destroy(&vmaf->hip.frame)" not in backends
        or "vmaf_close_backends(vmaf)" not in owners
        or "feature_extractor_vector_close(" not in prepare
    ):
        failures.append(f"{LIBVMAF}: the shared frame is not destroyed after the extractors close")
    if "entry->fex.hip_frame = fex->hip_frame;" not in src[FEX]:
        failures.append(f"{FEX}: a refreshed extractor entry loses the shared frame")
    return failures


def _shared_frame_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    text = src[SHARED]
    upload = _function_body(text, "frame_upload")
    if "vmaf_hip_picture_upload(batch.todo, batch.count, stream)" not in upload or RAW_COPY.search(
        text
    ):
        failures.append(f"{SHARED}: a shared plane is uploaded without the waiting upload")
    if not _in_order(upload, "frame_fence(f)", "batch_add(&batch, sp,", "batch_add_expected(f,"):
        failures.append(f"{SHARED}: a slot is written before it was fenced")
    if "batch_add_expected(f, &batch)" not in upload:
        failures.append(f"{SHARED}: an upload leaves the planes of the frame before to later waits")
    fence = _function_body(text, "frame_fence")
    if not _in_order(fence, "f->readers[f->slot] == 0u", "hipDeviceSynchronize()"):
        failures.append(f"{SHARED}: a held slot is overwritten without waiting for the device")
    acquire = _function_body(text, "vmaf_hip_plane_source_acquire")
    if not _in_order(acquire, "source_release(src);", "frame_serves(frame,", "frame_upload("):
        failures.append(f"{SHARED}: a twin does not let go of its slot before it asks again")
    end = _function_body(text, "vmaf_hip_shared_frame_end")
    serves = _function_body(text, "frame_serves")
    if "frame->active = false;" not in end or "!f->active" not in serves:
        failures.append(f"{SHARED}: a plane is served from the pictures after the frame ended")
    if "plane_is_whole(" not in serves or "announced_picture(" not in serves:
        failures.append(f"{SHARED}: a request is served without checking picture and layout")
    begin = _function_body(text, "vmaf_hip_shared_frame_begin")
    if "uploaded = false;" not in begin or "frame->fenced = false;" not in begin:
        failures.append(f"{SHARED}: a new frame inherits the previous frame's uploads")
    if not _in_order(
        begin,
        "frame->wanted_before[i][p] = frame->wanted[i][p];",
        "frame->wanted[i][p] = false;",
    ):
        failures.append(f"{SHARED}: a plane stays expected after a frame nobody asked for it in")
    return failures


def _failures(src: dict[str, str]) -> list[str]:
    return _twin_failures(src) + _framework_failures(src) + _shared_frame_failures(src)


def _replace(src: dict[str, str], name: str, old: str, new: str) -> dict[str, str]:
    if old not in src[name]:
        raise AssertionError(f"planted regression anchor missing in {name}: {old!r}")
    out = dict(src)
    out[name] = src[name].replace(old, new, 1)
    return out


class HipSharedFrameContractTest(unittest.TestCase):
    def assert_detected(self, src: dict[str, str], needle: str) -> None:
        failures = _failures(src)
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_live_sources_keep_the_contract(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_twin_with_its_own_upload_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "integer_vif_hip.c",
            "    if (err == 0)\n        err = vif_hip_launch_scales(",
            "    if (err == 0)\n        err = vmaf_hip_picture_upload(NULL, 0u, 0u);\n"
            "    if (err == 0)\n        err = vif_hip_launch_scales(",
        )
        self.assert_detected(src, "uploads a picture plane without the shared frame")

    def test_twin_that_stops_asking_is_detected(self) -> None:
        src = _sources()
        name = "float_psnr_hip.c"
        self.assertIn("vmaf_hip_plane_source_acquire_luma(", src[name])
        src[name] = src[name].replace("vmaf_hip_plane_source_acquire_luma(", "fpsnr_stage(")
        self.assert_detected(src, "does not ask the context's shared frame")

    def test_twin_that_never_closes_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "float_moment_hip.c",
            "    vmaf_hip_plane_source_close(&s->planes);\n",
            "",
        )
        self.assert_detected(src, "never closes its plane source")

    def test_frame_ended_before_dispatch_is_detected(self) -> None:
        text = _sources()[LIBVMAF]
        end = "    vmaf_hip_shared_frame_end(vmaf->hip.frame);\n"
        dispatch = "    err = read_pictures_dispatch_extractors(vmaf, fr, index);\n"
        self.assertIn(end, text)
        self.assertIn(dispatch, text)
        src = _sources()
        src[LIBVMAF] = text.replace(end, "", 1).replace(dispatch, end + dispatch, 1)
        self.assert_detected(src, "announced before and ended after dispatch")

    def test_unannounced_frame_is_detected(self) -> None:
        src = _replace(
            _sources(),
            LIBVMAF,
            "    return vmaf_hip_shared_frame_begin(vmaf->hip.frame, fr->ref, fr->dist);",
            "    (void)fr;\n    return 0;",
        )
        # The HAVE_CUDA branch still announces; drop it as well.
        src = _replace(
            src,
            LIBVMAF,
            "    return vmaf_hip_shared_frame_begin(vmaf->hip.frame, &fr->ref_host, "
            "&fr->dist_host);",
            "    return 0;",
        )
        self.assert_detected(src, "no longer announces the frame's pictures")

    def test_leaked_shared_frame_is_detected(self) -> None:
        src = _replace(
            _sources(),
            LIBVMAF,
            "    vmaf_hip_shared_frame_destroy(&vmaf->hip.frame);\n",
            "",
        )
        self.assert_detected(src, "not destroyed after the extractors close")

    def test_twin_without_the_frame_handle_is_detected(self) -> None:
        src = _replace(
            _sources(),
            LIBVMAF,
            "    fex_ctx->fex->hip_frame = vmaf->hip.frame;",
            "    fex_ctx->fex->hip_frame = NULL;",
        )
        self.assert_detected(src, "not handed the context's shared frame")

    def test_stale_entry_handle_is_detected(self) -> None:
        src = _replace(_sources(), FEX, "    entry->fex.hip_frame = fex->hip_frame;\n", "")
        self.assert_detected(src, "loses the shared frame")

    def test_upload_without_the_wait_is_detected(self) -> None:
        src = _replace(
            _sources(),
            SHARED,
            "    err = vmaf_hip_picture_upload(batch.todo, batch.count, stream);",
            "    err = (hipMemcpy2DAsync(batch.todo[0].dst, batch.todo[0].dst_pitch,\n"
            "                            batch.todo[0].pic->data[0],\n"
            "                            (size_t)batch.todo[0].pic->stride[0],\n"
            "                            batch.todo[0].row_bytes, batch.todo[0].rows,\n"
            "                            hipMemcpyHostToDevice, NULL) == hipSuccess)\n"
            "              ? 0\n"
            "              : -EIO;",
        )
        self.assert_detected(src, "without the waiting upload")

    def test_unfenced_slot_is_detected(self) -> None:
        src = _replace(
            _sources(),
            SHARED,
            "            err = frame_fence(f);\n            if (err == 0)\n",
            "            err = 0;\n            if (err == 0)\n",
        )
        self.assert_detected(src, "written before it was fenced")

    def test_fence_that_does_not_wait_is_detected(self) -> None:
        src = _replace(
            _sources(),
            SHARED,
            "    return vmaf_hip_rc_to_errno(hipDeviceSynchronize());",
            "    return 0;",
        )
        self.assert_detected(src, "without waiting for the device")

    def test_hold_kept_across_the_acquire_is_detected(self) -> None:
        text = _sources()[SHARED]
        release = "    source_release(src);\n\n    if (!frame_serves("
        self.assertIn(release, text)
        src = _sources()
        src[SHARED] = text.replace(release, "    if (!frame_serves(", 1)
        self.assert_detected(src, "does not let go of its slot")

    def test_frame_served_after_its_end_is_detected(self) -> None:
        src = _replace(_sources(), SHARED, "    frame->active = false;\n", "")
        self.assert_detected(src, "after the frame ended")

    def test_unchecked_request_is_detected(self) -> None:
        src = _replace(
            _sources(),
            SHARED,
            " ||\n            !plane_is_whole(&planes[i]))",
            ")",
        )
        self.assert_detected(src, "without checking picture and layout")

    def test_upload_without_the_expected_planes_is_detected(self) -> None:
        src = _replace(
            _sources(),
            SHARED,
            "    if (err == 0 && batch.count != 0u)\n        err = batch_add_expected(f, &batch);\n",
            "",
        )
        self.assert_detected(src, "leaves the planes of the frame before to later waits")

    def test_expectation_that_never_decays_is_detected(self) -> None:
        src = _replace(_sources(), SHARED, "            frame->wanted[i][p] = false;\n", "")
        self.assert_detected(src, "stays expected after a frame nobody asked for it in")

    def test_inherited_uploads_are_detected(self) -> None:
        src = _replace(
            _sources(),
            SHARED,
            "            frame->plane[frame->slot][i][p].uploaded = false;\n",
            "",
        )
        self.assert_detected(src, "inherits the previous frame's uploads")


if __name__ == "__main__":
    unittest.main()
