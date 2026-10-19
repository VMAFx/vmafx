#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The generated Python binding against the library Meson built (ADR-1852).

Meson passes the shared library in $VMAFX_LIBRARY (libvmafx), the libvmaf
compat library in $VMAF_COMPAT_LIBRARY (libvmaf.so.3, ADR-1852 decision D3)
and the binding's directory in $PYTHONPATH. The cases cover the slice the binding wraps (version, ABI,
context, provenance, extractor info, feature score) and the two refusals the
binding owns: no implicit library search, and a struct layout that disagrees
with the definition.
"""

from __future__ import annotations

import ctypes
import errno
import inspect
import os
import unittest

from vmafx import _api as vmafx


class BindingTest(unittest.TestCase):
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
        # One Python name for vmafx_context_feature_provenance (by index) and
        # vmafx_feature_provenance (by name) left only the second reachable
        # (T-VMAFX-PYTHON-METHOD-SHADOWED-2026-10-07).
        by_index = inspect.signature(vmafx.Context.feature_provenance_at).parameters
        by_name = inspect.signature(vmafx.Context.feature_provenance).parameters
        self.assertIn("index", by_index)
        self.assertIn("feature", by_name)

    def test_version_and_abi(self) -> None:
        self.assertTrue(self.library.version_string())
        self.assertEqual(self.library.abi_version(), vmafx.ABI_VERSION)
        self.assertEqual(self.library.status_name(vmafx.Status.E_RANGE), "VMAFX_E_RANGE")

    def test_context_provenance(self) -> None:
        with self.library.context() as context:
            provenance = context.provenance()
        self.assertEqual(provenance.n_extractors, 0)
        self.assertEqual(provenance.version, self.library.version_string())
        self.assertEqual(provenance.active_backend, vmafx.Backend.CPU)

    def test_named_error(self) -> None:
        with self.library.context() as context, self.assertRaises(vmafx.VmafxError) as caught:
            context.extractor_info(0)
        self.assertEqual(caught.exception.status, vmafx.Status.E_NOTFOUND)
        self.assertEqual(caught.exception.subject, "index")
        self.assertEqual(caught.exception.errno, -errno.ENOENT)

    def test_config_refused(self) -> None:
        config = vmafx.ContextConfig(
            log_level=99, n_threads=0, n_subsample=0, cpumask=0, gpumask=0, import_retry_wait_ns=0
        )
        with self.assertRaises(vmafx.VmafxError) as caught:
            self.library.context(config)
        self.assertEqual(caught.exception.subject, "config.log_level")

    def test_feature_score_of_imported_value(self) -> None:
        # The libvmaf function lives in the compat library, on the same context.
        raw = ctypes.CDLL(os.environ["VMAF_COMPAT_LIBRARY"])
        raw.vmaf_import_feature_score.argtypes = (
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_double,
            ctypes.c_uint,
        )
        raw.vmaf_import_feature_score.restype = ctypes.c_int
        with self.library.context() as context:
            handle = context.libvmaf_handle()
            self.assertEqual(raw.vmaf_import_feature_score(handle, b"bound", 4.25, 7), 0)
            score = context.feature_score("bound", 7)
        self.assertEqual(score.value, 4.25)
        self.assertEqual(score.index, 7)
        self.assertIsNone(score.extractor)

    def test_core_context_calls(self) -> None:
        """RC4 WP2: imported scores, pooling, context options, built-in models."""
        with self.library.context() as context:
            for index, value in enumerate((1.0, 3.0, 2.0)):
                context.import_score("imported", index, value)
            pooled = context.feature_score_pooled("imported", vmafx.Pool.MAX, 0, 2)
            context.set_option("perceptual_weight", "1")
            with self.assertRaises(vmafx.VmafxError) as caught:
                context.set_option("no_such_option", "1")
        self.assertEqual(pooled.value, 3.0)
        self.assertEqual((pooled.first, pooled.last, pooled.pool), (0, 2, vmafx.Pool.MAX))
        self.assertEqual(caught.exception.status, vmafx.Status.E_NOTFOUND)
        self.assertEqual(caught.exception.subject, "no_such_option")
        self.assertEqual(self.library.model_default_version(), "vmaf_v1.0.16_3d0h")
        self.assertTrue(self.library.model_builtin_next(None))

    def test_frame_colour_and_sample_range(self) -> None:
        """ADR-2094: a frame descriptor carries its colour to C, the context's
        default colour is a raw call, check_sample_range a context option."""
        pq = vmafx.Color(
            range=vmafx.ColorRange.LIMITED,
            primaries=vmafx.ColorPrimaries.BT2020,
            trc=vmafx.ColorTransfer.SMPTE2084,
            matrix=vmafx.ColorMatrix.BT2020_NCL,
        )
        desc = vmafx.FrameDesc(pix_fmt=vmafx.PixelFormat.YUV420P, bpc=10, w=64, h=48, color=pq)
        raw_desc = desc.to_c()
        self.assertEqual(raw_desc.struct_size, ctypes.sizeof(vmafx.VmafxFrameDesc))
        self.assertEqual(vmafx.FrameDesc.from_c(raw_desc), desc)
        with self.library.context() as context:
            context.set_option("check_sample_range", "1")
            with self.assertRaises(vmafx.VmafxError) as caught:
                context.set_option("check_sample_range", "maybe")
            color = pq.to_c()
            status = self.library.raw.vmafx_context_set_default_color(
                context._handle, ctypes.byref(color), None, None
            )
        self.assertEqual(caught.exception.status, vmafx.Status.E_INVALID)
        self.assertEqual(status, vmafx.Status.OK)

    def test_layout_check_refuses_drift(self) -> None:
        size, offsets = vmafx.LAYOUT[vmafx.VmafxScore]
        vmafx.LAYOUT[vmafx.VmafxScore] = (size + 8, offsets)
        try:
            with self.assertRaisesRegex(ImportError, "VmafxScore: size"):
                vmafx._check_layout()
        finally:
            vmafx.LAYOUT[vmafx.VmafxScore] = (size, offsets)


def sanitizer_skip_reason() -> str | None:
    """Why the binding cannot be loaded here, or None.

    A library built with -Db_sanitize needs its sanitizer runtime in the
    process; ctypes loads it into an uninstrumented interpreter, which fails
    with an undefined `__tsan_*` / `__asan_*` symbol. Meson passes the option
    in $VMAFX_SANITIZE.
    """
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
