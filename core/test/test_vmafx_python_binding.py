#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The generated Python binding against the library Meson built (ADR-1852, #2434).

Meson passes the shared library in $VMAFX_LIBRARY (libvmafx), the libvmaf
compat library in $VMAF_COMPAT_LIBRARY (libvmaf.so.3, ADR-1852 decision D3)
and the binding's directory in $PYTHONPATH.
"""

from __future__ import annotations

import array
import ast
import ctypes
import errno
import gc
import inspect
import json
import os
import subprocess
import tempfile
import unittest
import weakref
from pathlib import Path
from unittest import mock

import vmafx as package
from vmafx import _api as vmafx

REPO_ROOT = Path(__file__).resolve().parents[2]
WRAPPED_CLASSES = (
    vmafx.Library,
    vmafx.Context,
    vmafx.Options,
    vmafx.Device,
    vmafx.Frame,
    vmafx.FramePool,
    vmafx.FrameConverter,
    vmafx.Model,
    vmafx.ModelSet,
    vmafx.ReportFile,
    vmafx.Window,
    vmafx.WindowClock,
    vmafx.DnnSession,
    vmafx.McpServer,
)


class _TrackedByteArray(bytearray):
    """Subclass of bytearray that supports weakref for buffer-lifetime verification."""


def _untested_methods(expected: set[str], called: set[str]) -> list[str]:
    """Return sorted method names in `expected` that no test call in `called` names."""
    return sorted(expected - called)


class LoaderAndLayoutTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.library = vmafx.Library(os.environ["VMAFX_LIBRARY"])

    def test_no_implicit_library_search(self) -> None:
        saved = os.environ.pop("VMAFX_LIBRARY")
        try:
            with self.assertRaisesRegex(OSError, "VMAFX_LIBRARY"):
                vmafx.load()
        finally:
            os.environ["VMAFX_LIBRARY"] = saved

    def test_both_feature_provenance_lookups_are_bound(self) -> None:
        by_index = inspect.signature(vmafx.Context.feature_provenance_at).parameters
        by_name = inspect.signature(vmafx.Context.feature_provenance).parameters
        self.assertIn("index", by_index)
        self.assertIn("feature", by_name)

    def test_package_exports_what_the_bindings_guide_uses(self) -> None:
        for name in (
            "Library",
            "Pool",
            "PooledScore",
            "VmafxError",
            "VmafxPending",
            "NOT_WRAPPED",
            "WRAPPED_FUNCTIONS",
        ):
            self.assertIs(getattr(package, name), getattr(vmafx, name), name)
            self.assertIn(name, package.__all__)
        with self.library.context() as context:
            context.import_score("external", 0, 1.5)
            pooled = context.feature_score_pooled("external", package.Pool.MEAN, 0, 0)
        self.assertIsInstance(pooled, package.PooledScore)
        self.assertEqual(pooled.value, 1.5)

    def test_layout_check_refuses_drift_for_every_struct(self) -> None:
        self.assertGreaterEqual(len(vmafx.LAYOUT), 32)
        for struct, (size, offsets) in list(vmafx.LAYOUT.items()):
            vmafx.LAYOUT[struct] = (size + 8, offsets)
            try:
                with self.assertRaisesRegex(ImportError, f"{struct.__name__}: size"):
                    vmafx._check_layout()
            finally:
                vmafx.LAYOUT[struct] = (size, offsets)


class LibraryAndDeviceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.library = vmafx.Library(os.environ["VMAFX_LIBRARY"])

    def test_version_abi_status_backend_and_models(self) -> None:
        self.assertIsNotNone(self.library.raw)
        self.assertTrue(self.library.version_string())
        self.assertEqual(self.library.abi_version(), vmafx.ABI_VERSION)
        self.assertEqual(self.library.status_name(vmafx.Status.E_RANGE), "VMAFX_E_RANGE")
        self.assertEqual(self.library.backend_name(vmafx.Backend.CPU), "cpu")
        self.assertEqual(self.library.model_default_version(), "vmaf_v1.0.16_3d0h")
        first = self.library.model_builtin_next(None)
        self.assertIsNotNone(first)
        assert first is not None
        second = self.library.model_builtin_next(first)
        self.assertIsNotNone(second)

    def test_device_queries_and_handle_lifecycle(self) -> None:
        self.assertEqual(self.library.device_count(vmafx.Backend.CPU), 1)
        info = self.library.device_info(vmafx.Backend.CPU, 0)
        self.assertEqual(info.backend, vmafx.Backend.CPU)
        desc = vmafx.DeviceDesc(
            backend=vmafx.Backend.CPU,
            index=0,
            flags=0,
            external=(0, 0),
        )
        with self.library.device(desc) as dev:
            self.assertEqual(dev.backend(), vmafx.Backend.CPU)
            self.assertEqual(dev.describe().backend, vmafx.Backend.CPU)
            with self.assertRaises(vmafx.VmafxError):
                dev.profile()
            clone = dev.ref()
            clone.unref()
        dev.close()

    def test_options_lifecycle(self) -> None:
        with self.library.options({"adm_norm_view_dist": "3.0"}) as opts:
            opts.set("adm_ref_display_height", "1080")
        opts.close()


class ContextCloseAndErrorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.library = vmafx.Library(os.environ["VMAFX_LIBRARY"])

    def test_named_error_and_out_of_range_config(self) -> None:
        with self.library.context() as context, self.assertRaises(vmafx.VmafxError) as caught:
            context.extractor_info(0)
        self.assertEqual(caught.exception.status, vmafx.Status.E_NOTFOUND)
        self.assertEqual(caught.exception.subject, "index")
        self.assertEqual(caught.exception.errno, -errno.ENOENT)
        self.assertEqual(caught.exception.subject_kind, vmafx.SubjectKind.EXTRACTOR)
        self.assertEqual(caught.exception.function, "vmafx_context_extractor_info")
        config = vmafx.ContextConfig(
            log_level=vmafx.LogLevel.NONE,
            n_threads=0,
            n_subsample=0,
            cpumask=0,
            gpumask=0,
            import_retry_wait_ns=1 << 62,
        )
        with self.assertRaises(vmafx.VmafxError) as range_err:
            self.library.context(config)
        self.assertEqual(range_err.exception.status, vmafx.Status.E_RANGE)
        self.assertEqual(range_err.exception.subject, "config.import_retry_wait_ns")

    def test_double_close_and_retry_safe_close(self) -> None:
        ctx = self.library.context()
        ctx.close()
        ctx.close()  # second close after success is a no-op
        ctx2 = self.library.context()
        real_destroy = self.library.raw.vmafx_context_destroy
        calls = 0

        def flaky_destroy(handle: ctypes.c_void_p, error_ptr: object) -> int:
            nonlocal calls
            calls += 1
            if calls == 1:
                return int(vmafx.Status.E_BUSY)
            return int(real_destroy(handle, error_ptr))

        with mock.patch.object(
            self.library.raw, "vmafx_context_destroy", side_effect=flaky_destroy
        ):
            with self.assertRaises(vmafx.VmafxError) as err:
                ctx2.close()
            self.assertEqual(err.exception.status, vmafx.Status.E_BUSY)
            self.assertTrue(bool(ctx2._handle))
            ctx2.close()
            self.assertFalse(bool(ctx2._handle))
        ctx3 = self.library.context()
        del_calls = 0

        def always_fail(_handle: ctypes.c_void_p, _error_ptr: object) -> int:
            nonlocal del_calls
            del_calls += 1
            return int(vmafx.Status.E_BUSY)

        with mock.patch.object(self.library.raw, "vmafx_context_destroy", side_effect=always_fail):
            ctx3.__del__()
            self.assertEqual(del_calls, 2)
        ctx3.close()

    def test_log_callback_and_exception_propagation(self) -> None:
        seen: list[tuple[int, str]] = []
        cfg = vmafx.ContextConfig(
            log_level=vmafx.LogLevel.DEBUG,
            n_threads=0,
            n_subsample=0,
            cpumask=0,
            gpumask=0,
            import_retry_wait_ns=0,
        )
        with self.library.context(
            cfg, log_callback=lambda lvl, msg: seen.append((lvl, msg))
        ) as ctx:
            ctx.use_feature("psnr")

        def boom(_lvl: int, _msg: str) -> None:
            raise RuntimeError("log callback boom")

        state = vmafx._CallbackState()
        thunk, thunk_state = vmafx._make_log_thunk(boom)
        assert thunk_state is not None
        thunk(int(vmafx.LogLevel.INFO), b"hello", None)
        with self.assertRaisesRegex(RuntimeError, "log callback boom"):
            thunk_state.reraise()
        self.assertIsNone(state.exc)


class HostFrameAndPoolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.library = vmafx.Library(os.environ["VMAFX_LIBRARY"])
        cls.color = vmafx.Color(
            range=vmafx.ColorRange.LIMITED,
            primaries=vmafx.ColorPrimaries.BT709,
            trc=vmafx.ColorTransfer.BT709,
            matrix=vmafx.ColorMatrix.BT709,
        )
        cls.desc8 = vmafx.FrameDesc(
            pix_fmt=vmafx.PixelFormat.YUV420P, bpc=8, w=16, h=16, color=cls.color
        )

    def test_wrap_host_buffer_exporters_and_validation(self) -> None:
        y_bytes = b"\x80" * (16 * 16)
        u_ba = bytearray(b"\x80" * (8 * 8))
        v_mv = memoryview(b"\x00" * 16 + b"\x80" * (8 * 8))[16:]
        with self.library.frame_wrap_host(self.desc8, (y_bytes, u_ba, v_mv)) as frame:
            planes = frame.planes()
            self.assertEqual(planes.n_planes, 3)
            self.assertEqual(planes.w[:3], (16, 8, 8))
        arr_y = array.array("B", [128] * (16 * 16))
        arr_uv = array.array("B", [128] * (8 * 8))
        with vmafx.Frame.wrap_host(self.library.raw, self.desc8, (arr_y, arr_uv, arr_uv)) as f2:
            self.assertEqual(len(f2.plane_buffers()), 3)
        with self.assertRaisesRegex(ValueError, r"plane\[1\] buffer length"):
            self.library.frame_wrap_host(self.desc8, (y_bytes, b"short", v_mv))
        with self.assertRaisesRegex(ValueError, r"plane\[0\] stride"):
            self.library.frame_wrap_host(self.desc8, (y_bytes, u_ba, v_mv), strides=(8, 8, 8))
        with self.assertRaisesRegex(ValueError, r"plane\[2\] count"):
            self.library.frame_wrap_host(self.desc8, (y_bytes, u_ba))

    def test_exporter_stays_alive_until_release(self) -> None:
        released = False

        def on_rel() -> None:
            nonlocal released
            released = True

        exp_y = _TrackedByteArray(b"\x80" * (16 * 16))
        exp_u = _TrackedByteArray(b"\x80" * (8 * 8))
        exp_v = _TrackedByteArray(b"\x80" * (8 * 8))
        ref_y = weakref.ref(exp_y)
        frame = self.library.frame_wrap_host(self.desc8, (exp_y, exp_u, exp_v), on_release=on_rel)
        del exp_y, exp_u, exp_v
        gc.collect()
        self.assertIsNotNone(ref_y())
        extra = frame.ref()
        frame.unref()
        gc.collect()
        self.assertIsNotNone(ref_y())
        self.assertFalse(released)
        extra.close()
        gc.collect()
        self.assertTrue(released)
        self.assertIsNone(ref_y())

    def test_create_host_fences_pool_and_converter(self) -> None:
        with self.library.frame_create_host(self.desc8) as frame:
            for buf in frame.plane_buffers():
                buf[:] = b"\x40" * len(buf)
            rel_fence = frame.release_fence(vmafx.FenceKind.HOST)
            sig_fence = self.library.fence_create(vmafx.FenceKind.HOST)
            with self.assertRaises(vmafx.VmafxError):
                frame.signal_on_release(sig_fence)
        self.library.fence_wait(rel_fence, 1_000_000)
        self.library.fence_destroy(rel_fence)
        self.library.fence_destroy(sig_fence)
        manual_fence = self.library.fence_create(vmafx.FenceKind.HOST)
        self.library.fence_signal(manual_fence)
        self.library.fence_wait(manual_fence, 1_000_000)
        self.library.fence_destroy(manual_fence)
        with self.library.frame_pool(self.desc8, 2) as pool, pool.acquire() as pooled_frame:
            self.assertEqual(pooled_frame.planes().w[0], 16)
        conv_desc = vmafx.ConvertDesc(
            src_pix_fmt=vmafx.PixelFormat.YUV420P,
            src_bpc=8,
            src_w=16,
            src_h=16,
            src_color=self.color,
            dst_pix_fmt=vmafx.PixelFormat.YUV420P,
            dst_bpc=8,
            dst_w=16,
            dst_h=16,
            dst_color=self.color,
            filter=vmafx.ResampleFilter.BILINEAR,
        )
        with vmafx.Frame.create_host(self.library.raw, self.desc8) as src:
            try:
                conv = self.library.frame_converter(conv_desc)
            except vmafx.VmafxError as exc:
                self.assertEqual(exc.status, vmafx.Status.E_NOTSUP)
                dummy_conv = vmafx.FrameConverter(self.library.raw, _handle=ctypes.c_void_p())
                with self.assertRaises(vmafx.VmafxError):
                    dummy_conv.convert(src)
                dummy_conv.close()
            else:
                with conv, conv.convert(src) as dst:
                    self.assertEqual(dst.planes().w[0], 16)


class ModelAndScoringTest(unittest.TestCase):
    def test_raw_model_config_keeps_the_log_callback(self) -> None:
        raw = vmafx.VmafxModelConfig()
        raw.struct_size = ctypes.sizeof(raw)
        built, _, thunk, _ = vmafx._raw_model_config(raw, lambda _level, _message: None)
        self.assertIs(built, raw)
        wired = ctypes.cast(raw.log_callback, ctypes.c_void_p).value
        self.assertEqual(wired, ctypes.cast(thunk, ctypes.c_void_p).value)

    @classmethod
    def setUpClass(cls) -> None:
        cls.library = vmafx.Library(os.environ["VMAFX_LIBRARY"])
        cls.color = vmafx.Color(
            range=vmafx.ColorRange.LIMITED,
            primaries=vmafx.ColorPrimaries.BT709,
            trc=vmafx.ColorTransfer.BT709,
            matrix=vmafx.ColorMatrix.BT709,
        )
        cls.desc8 = vmafx.FrameDesc(
            pix_fmt=vmafx.PixelFormat.YUV420P, bpc=8, w=64, h=64, color=cls.color
        )

    def test_model_and_model_set_methods(self) -> None:
        with (
            self.library.model_load("vmaf_v0.6.1") as model,
            self.library.options({"adm_norm_view_dist": "3.0"}) as opts,
        ):
            self.assertTrue(model.name())
            self.assertGreater(model.feature_count(), 0)
            self.assertTrue(model.feature_name(0))
            self.assertTrue(model.hash())
            model.override_feature("adm", opts)
            clone = model.ref()
            clone.unref()
        with self.library.model_load_spec("version=vmaf_v0.6.1:name=spec_vmaf") as spec_model:
            self.assertEqual(spec_model.name(), "spec_vmaf")
        with vmafx.Model.load_spec(self.library.raw, "version=vmaf_v0.6.1:name=spec2") as spec2:
            self.assertEqual(spec2.name(), "spec2")
        with self.assertRaises(vmafx.VmafxError):
            vmafx.Model.load_file(self.library.raw, "/nonexistent/model.json")
        with self.assertRaises(vmafx.VmafxError):
            self.library.model_load_file("/nonexistent/model.json")
        with (
            self.library.model_set_load("vmaf_b_v0.6.3") as mset,
            self.library.options({"adm_norm_view_dist": "3.0"}) as opts,
        ):
            self.assertGreater(mset.size(), 1)
            self.assertTrue(mset.hash())
            mset.override_feature("adm", opts)
            lead = mset.lead()
            self.assertIsNotNone(lead)
            if lead is not None:
                lead.close()
            ms_clone = mset.ref()
            ms_clone.unref()
        with self.assertRaises(vmafx.VmafxError):
            self.library.model_set_load_file("/nonexistent/model_set.json")

    def test_context_scoring_provenance_and_reports(self) -> None:
        with (
            self.library.device() as dev,
            self.library.model_load("vmaf_v0.6.1") as model,
            self.library.model_set_load("vmaf_b_v0.6.3") as mset,
            self.library.context() as ctx,
        ):
            ctx.use_device(dev)
            ctx.use_model(model)
            ctx.use_model_set(mset)
            ctx.use_feature("psnr")
            ctx.use_feature_spec("name=float_ssim")
            self.assertGreater(ctx.extractor_count(), 0)
            self.assertTrue(ctx.extractor_info(0).name)
            self.assertEqual(
                ctx.feature_resolve("psnr", frame=self.desc8).backend, vmafx.Backend.CPU
            )
            self.assertGreaterEqual(ctx.frame_retention(), 1)
            self.assertGreaterEqual(ctx.max_in_flight(), 1)
            self.assertEqual(ctx.backend(), vmafx.Backend.CPU)
            self.assertTrue(ctx.libvmaf_handle())
            ctx.set_option("check_sample_range", "1")
            ctx.set_default_color(self.color, self.color)
            ctx.preallocate(self.desc8, 2)
            with ctx.acquire_frame() as ref_frame, ctx.acquire_frame() as dist_frame:
                for buf in ref_frame.plane_buffers():
                    buf[:] = b"\x80" * len(buf)
                for buf in dist_frame.plane_buffers():
                    buf[:] = b"\x7f" * len(buf)
                ctx.admit(ref_frame)
                with self.assertRaises(vmafx.VmafxError):
                    ctx.attach_sidedata(0, b"pelorus")
                ctx.submit(ref_frame, dist_frame, 0)
            ctx.flush()
            self.assertGreaterEqual(ctx.score_frame(model, 0).value, 0.0)
            self.assertGreaterEqual(ctx.score_frame_model_set(mset, 0).bagging, 0.0)
            self.assertGreaterEqual(ctx.score_pooled(model, vmafx.Pool.MEAN, 0, 0).value, 0.0)
            self.assertGreaterEqual(
                ctx.score_pooled_model_set(mset, vmafx.Pool.MEAN, 0, 0).bagging, 0.0
            )
            self.assertGreaterEqual(ctx.feature_score("psnr_y", 0).value, 0.0)
            self.assertGreaterEqual(
                ctx.feature_score_pooled("psnr_y", vmafx.Pool.MEAN, 0, 0).value, 0.0
            )
            self._check_provenance_and_reports(ctx)

    def _check_provenance_and_reports(self, ctx: vmafx.Context) -> None:
        ctx.annotate("clip", "demo")
        ctx.set_encode_record("sha256:" + "0" * 64)
        self.assertEqual(ctx.annotation(0).key, "clip")
        self.assertTrue(ctx.model_provenance(0).name)
        self.assertTrue(ctx.feature_provenance_at(0).feature)
        self.assertEqual(ctx.feature_provenance("psnr_y").feature, "psnr_y")
        self.assertEqual(ctx.provenance().n_frames, 1)
        self.assertIn("abi_major", ctx.provenance_json())
        with tempfile.TemporaryDirectory() as tmp:
            report_path = str(Path(tmp) / "report.json")
            ctx.report_write(report_path, vmafx.ReportFormat.JSON, 0, None)
            with (
                self.library.report_open(report_path) as rep1,
                self.library.report_open(report_path) as rep2,
            ):
                self.assertTrue(rep1.field("version"))
                rep1.verify(rep2)
                self.library.report_verify(rep1, rep2)


class WindowAndClockTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.library = vmafx.Library(os.environ["VMAFX_LIBRARY"])

    def test_window_callback_gc_survival_and_poll_wait(self) -> None:
        results: list[vmafx.WindowResult] = []
        with self.library.context() as ctx:
            win = ctx.window_submit(
                target=vmafx.WindowTarget.FEATURE,
                pool_mask=vmafx.PoolMask.MEAN,
                first=0,
                last=1,
                feature="ext",
                on_complete=results.append,
            )
            gc.collect()
            with self.assertRaises(vmafx.VmafxPending):
                win.poll()
            ctx.import_score("ext", 0, 10.0)
            ctx.import_score("ext", 1, 20.0)
            ctx.flush()
            waited = win.wait(1_000_000_000)
            polled = win.poll()
            win.release()
            win.close()
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].value[vmafx.Pool.MEAN], 15.0)
        self.assertEqual(polled.value[vmafx.Pool.MEAN], 15.0)
        self.assertEqual(waited.value[vmafx.Pool.MEAN], 15.0)

    def test_window_callback_exception_is_reraised_on_calling_thread(self) -> None:
        def fail_cb(_res: vmafx.WindowResult) -> None:
            raise ValueError("window callback failed")

        with (
            self.assertRaisesRegex(ValueError, "window callback failed"),
            self.library.context() as ctx,
        ):
            win = ctx.window_submit(
                target=vmafx.WindowTarget.FEATURE,
                pool_mask=vmafx.PoolMask.MEAN,
                first=0,
                last=0,
                feature="ext",
                on_complete=fail_cb,
            )
            ctx.import_score("ext", 0, 5.0)
            ctx.flush()
            win.wait(1_000_000_000)
            win.close()

    def test_window_clock(self) -> None:
        cfg = vmafx.WindowClockConfig(n_stats=0.0, n_stats_frames=2)
        with self.library.window_clock(cfg) as clock:
            span0 = clock.frame(0, 0)
            span1 = clock.frame(1, 10_000_000)
            span2 = clock.frame(2, 20_000_000)
            tail = clock.finish()
        self.assertIsNone(span0)
        self.assertIsNone(span1)
        assert span2 is not None
        assert tail is not None
        self.assertEqual((span2.first, span2.last), (0, 1))
        self.assertEqual((tail.first, tail.last), (2, 2))


class DnnAndMcpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.library = vmafx.Library(os.environ["VMAFX_LIBRARY"])

    def test_dnn_surface(self) -> None:
        self.assertIsInstance(self.library.dnn_available(), bool)
        with self.assertRaises(vmafx.VmafxError):
            self.library.dnn_verify_signature("/nonexistent.onnx", None)
        with self.assertRaises(vmafx.VmafxError):
            self.library.dnn_session("/nonexistent.onnx")
        dummy = vmafx.DnnSession(self.library.raw, _handle=ctypes.c_void_p())
        self.assertIsNone(dummy.runtime_device())
        src8 = b"\x00" * 16
        dst_f = bytearray(16 * 4)
        with self.assertRaises(vmafx.VmafxError):
            dummy.run_luma8(src8, 4, 4, 4, dst_f, 16)
        with self.assertRaises(vmafx.VmafxError):
            dummy.run_plane16(b"\x00" * 32, 8, 4, 4, 10, dst_f, 16)
        with self.assertRaises(vmafx.VmafxError):
            dummy.run([("in", dst_f, (1, 4))], [("out", dst_f)])
        dummy.close()
        with self.library.context() as ctx:
            self.assertFalse(ctx.is_codec_aware())
            with self.assertRaises(vmafx.VmafxError):
                ctx.use_tiny_model("/nonexistent.onnx")
            with self.assertRaises(vmafx.VmafxError):
                ctx.set_codec_context("h264", "medium", 23)
            ctx.set_tiny_resize(vmafx.DnnResize.BILINEAR)

    def test_mcp_surface(self) -> None:
        avail = self.library.mcp_available()
        self.assertIsInstance(avail, bool)
        self.assertIsInstance(self.library.mcp_transport_available(vmafx.McpTransport.STDIO), bool)
        if not avail:
            with self.library.context() as ctx, self.assertRaises(vmafx.VmafxError):
                ctx.mcp_server()
        srv = vmafx.McpServer(self.library.raw, _handle=ctypes.c_void_p())
        with self.assertRaises(vmafx.VmafxError):
            srv.start_sse(vmafx.McpSseConfig(port=0, path="/sse"))
        with self.assertRaises(vmafx.VmafxError):
            srv.start_uds("/nonexistent/vmafx.sock")
        with self.assertRaises(vmafx.VmafxError):
            srv.start_stdio(vmafx.McpStdioConfig(fd_in=-1, fd_out=-1))
        with self.assertRaises(vmafx.VmafxError):
            srv.stop()
        srv.close()


REQUIRE_PAIR_ENV = "VMAFX_REQUIRE_NETFLIX_PAIR"


def _missing_inputs(paths: tuple[Path, ...]) -> list[str]:
    return [str(path) for path in paths if not path.is_file()]


class NetflixBitExactTest(unittest.TestCase):
    """Bit-exact float.hex() parity with C vmafx_score_contract on Netflix 576x324 (#2434)."""

    YUV_DIR = Path(
        os.environ.get("VMAFX_YUV_DIR", REPO_ROOT / "python" / "test" / "resource" / "yuv")
    )
    REF_YUV = YUV_DIR / "src01_hrc00_576x324.yuv"
    DIST_YUV = YUV_DIR / "src01_hrc01_576x324.yuv"
    MODEL_JSON = REPO_ROOT / "model" / "vmaf_v1.0.16" / "vmaf_v1.0.16_3d0h.json"

    def _require_inputs(self, paths: tuple[Path, ...]) -> None:
        missing = _missing_inputs(paths)
        if not missing:
            return
        if os.environ.get(REQUIRE_PAIR_ENV):
            self.fail(f"{REQUIRE_PAIR_ENV} is set and inputs are missing: {missing}")
        self.skipTest(f"missing inputs: {missing}")

    def test_missing_input_fails_when_the_pair_is_required(self) -> None:
        absent = (REPO_ROOT / "no-such-netflix-pair.yuv",)
        with mock.patch.dict(os.environ, {REQUIRE_PAIR_ENV: "1"}):
            with self.assertRaisesRegex(self.failureException, "no-such-netflix-pair"):
                self._require_inputs(absent)
        with mock.patch.dict(os.environ, {REQUIRE_PAIR_ENV: ""}):
            with self.assertRaises(unittest.SkipTest):
                self._require_inputs(absent)

    def test_netflix_576x324_matches_c_api_float_hex(self) -> None:
        lib_path = Path(os.environ["VMAFX_LIBRARY"]).resolve()
        contract_bin = lib_path.parent.parent / "test" / "vmafx_score_contract"
        self._require_inputs((self.REF_YUV, self.DIST_YUV, self.MODEL_JSON, contract_bin))
        proc = subprocess.run(  # noqa: S603 -- trusted build artifact from Meson test dir
            [
                str(contract_bin),
                str(self.REF_YUV),
                str(self.DIST_YUV),
                "576",
                "324",
                "8",
                "vmaf_v1.0.16_3d0h",
                "0",
                "1",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        c_payload = json.loads(proc.stdout)
        w, h = 576, 324
        y_len = w * h
        uv_len = (w // 2) * (h // 2)
        frame_len = y_len + 2 * uv_len
        ref_bytes = self.REF_YUV.read_bytes()
        dist_bytes = self.DIST_YUV.read_bytes()
        n_frames = len(ref_bytes) // frame_len
        desc = vmafx.FrameDesc(
            pix_fmt=vmafx.PixelFormat.YUV420P,
            bpc=8,
            w=w,
            h=h,
            color=vmafx.Color(0, 0, 0, 0),
        )
        lib = vmafx.Library(str(lib_path))
        with lib.model_load_file(str(self.MODEL_JSON)) as model, lib.context() as ctx:
            ctx.use_model(model)
            for idx in range(n_frames):
                base = idx * frame_len
                r_mv = memoryview(ref_bytes)[base : base + frame_len]
                d_mv = memoryview(dist_bytes)[base : base + frame_len]
                r_planes = (r_mv[:y_len], r_mv[y_len : y_len + uv_len], r_mv[y_len + uv_len :])
                d_planes = (d_mv[:y_len], d_mv[y_len : y_len + uv_len], d_mv[y_len + uv_len :])
                with (
                    lib.frame_wrap_host(desc, r_planes) as rf,
                    lib.frame_wrap_host(desc, d_planes) as df,
                ):
                    ctx.submit(rf, df, idx)
            ctx.flush()
            pooled = ctx.score_pooled(model, vmafx.Pool.MEAN, 0, n_frames - 1)
        self.assertEqual(n_frames, c_payload["frames"])
        self.assertEqual(pooled.value.hex(), c_payload["vmaf_hex"])


class MechanicalCoverageTest(unittest.TestCase):
    """Every public method on the generated classes is called in this test file (#2434)."""

    def test_every_generated_method_is_named_in_tests(self) -> None:
        expected = {
            name
            for cls in WRAPPED_CLASSES
            for name, member in inspect.getmembers(cls)
            if not name.startswith("_") and (callable(member) or isinstance(member, property))
        }
        tree = ast.parse(Path(__file__).read_text(encoding="utf-8"))
        called = {
            node.attr
            for node in ast.walk(tree)
            if isinstance(node, ast.Attribute) and not node.attr.startswith("_")
        }
        self.assertEqual(_untested_methods(expected, called), [])
        self.assertEqual(
            _untested_methods(expected | {"planted_untested_method"}, called),
            ["planted_untested_method"],
        )


def sanitizer_skip_reason() -> str | None:
    """Why the binding cannot be loaded here, or None."""
    sanitize = os.environ.get("VMAFX_SANITIZE", "none")
    if sanitize in ("", "none"):
        return None
    return (
        f"SKIP: the library is built with -Db_sanitize={sanitize}; ctypes cannot load a "
        "sanitizer-instrumented library into the uninstrumented interpreter. The test runs "
        "in every build without a sanitizer."
    )


if __name__ == "__main__":
    REASON = sanitizer_skip_reason()
    if REASON is not None:
        print(REASON)
        raise SystemExit(77)
    unittest.main()
