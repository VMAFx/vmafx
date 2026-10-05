#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The generated Python binding against the library Meson built (ADR-1852).

Meson passes the shared library in $VMAFX_LIBRARY and the binding's directory
in $PYTHONPATH. The cases cover the slice the binding wraps (version, ABI,
context, provenance, extractor info, feature score) and the two refusals the
binding owns: no implicit library search, and a struct layout that disagrees
with the definition.
"""

from __future__ import annotations

import ctypes
import errno
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
        config = vmafx.ContextConfig(log_level=99, n_threads=0, n_subsample=0, cpumask=0, gpumask=0)
        with self.assertRaises(vmafx.VmafxError) as caught:
            self.library.context(config)
        self.assertEqual(caught.exception.subject, "config.log_level")

    def test_feature_score_of_imported_value(self) -> None:
        raw = self.library.raw
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

    def test_layout_check_refuses_drift(self) -> None:
        size, offsets = vmafx.LAYOUT[vmafx.VmafxScore]
        vmafx.LAYOUT[vmafx.VmafxScore] = (size + 8, offsets)
        try:
            with self.assertRaisesRegex(ImportError, "VmafxScore: size"):
                vmafx._check_layout()
        finally:
            vmafx.LAYOUT[vmafx.VmafxScore] = (size, offsets)


if __name__ == "__main__":
    unittest.main()
