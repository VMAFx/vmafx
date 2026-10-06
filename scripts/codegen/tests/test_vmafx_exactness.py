#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The exactness table generator (RC4 WP5) and its refusals.

The committed core/src/vmafx/exactness_gen.c equals the render of the live
parity-gate data; a fragment added or an alias with a different exact backend
set changes or refuses the render; ``--check`` fails on a one-byte change.
"""

from __future__ import annotations

import contextlib
import dataclasses
import io
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import cast

import support  # noqa: F401 -- puts scripts/codegen on sys.path
import vmafx_exactness as gen


def live(**overrides: object) -> str:
    data: dict[str, object] = {
        "fragments": gen.EXACT_TWIN_FRAGMENTS,
        "libm_twins": gen.LIBM_TWINS,
        "tolerance": gen.FEATURE_TOLERANCE,
        "default_tolerance": gen.DEFAULT_FP32_TOLERANCE,
        "aliases": gen.FEATURE_ALIASES,
        "suffix": gen.BACKEND_SUFFIX,
        "extractor_aliases": gen.BACKEND_EXTRACTOR_ALIASES,
    }
    data.update(overrides)
    render = cast(Callable[..., str], gen.render)
    return render(**data)


def fake(feature: str, backend: str) -> object:
    template = gen.EXACT_TWIN_FRAGMENTS[0]
    return dataclasses.replace(template, feature=feature, backend=backend)


class ExactnessGeneratorTest(unittest.TestCase):
    def test_committed_file_is_current(self) -> None:
        self.assertEqual(gen.OUTPUT.read_text(encoding="utf-8"), live())

    def test_drift_is_detected(self) -> None:
        kept = [f for f in gen.EXACT_TWIN_FRAGMENTS if (f.feature, f.backend) != ("adm", "cuda")]
        self.assertNotEqual(gen.OUTPUT.read_text(encoding="utf-8"), live(fragments=kept))

    def test_added_fragment_makes_the_twin_exact(self) -> None:
        before = live()
        after = live(fragments=[*gen.EXACT_TWIN_FRAGMENTS, fake("psnr", "metal")])
        self.assertIn(
            '"integer_psnr_metal", "psnr", VMAFX_BACKEND_METAL, VMAFX_EXACTNESS_TOLERANCE', before
        )
        self.assertIn(
            '"integer_psnr_metal", "psnr", VMAFX_BACKEND_METAL, VMAFX_EXACTNESS_EXACT', after
        )

    def test_alias_with_other_exact_backends_is_refused(self) -> None:
        extra = fake("motion_debug", "metal")
        with self.assertRaises(gen.ExactnessError) as caught:
            live(fragments=[*gen.EXACT_TWIN_FRAGMENTS, extra])
        self.assertIn("motion_debug", str(caught.exception))

    def test_alias_with_other_libm_backends_is_refused(self) -> None:
        libm = {**gen.LIBM_TWINS, "float_ssim_lcs": {"cuda": 1e-9}}
        with self.assertRaises(gen.ExactnessError) as caught:
            live(libm_twins=libm)
        self.assertIn("float_ssim_lcs", str(caught.exception))

    def test_unknown_feature_fragment_is_refused(self) -> None:
        with self.assertRaises(gen.ExactnessError) as caught:
            live(fragments=[*gen.EXACT_TWIN_FRAGMENTS, fake("nosuch", "cuda")])
        self.assertIn("nosuch", str(caught.exception))

    def test_rows_are_sorted_and_cover_every_pair(self) -> None:
        rows = gen.build_rows(
            gen.EXACT_TWIN_FRAGMENTS, gen.LIBM_TWINS, gen.FEATURE_TOLERANCE,
            gen.DEFAULT_FP32_TOLERANCE, gen.FEATURE_ALIASES, gen.BACKEND_SUFFIX,
            gen.BACKEND_EXTRACTOR_ALIASES,
        )  # fmt: skip
        names = [row[0] for row in rows]
        self.assertEqual(names, sorted(set(names)))
        bases = set(gen.FEATURE_TOLERANCE) - set(gen.FEATURE_ALIASES)
        self.assertEqual(len(rows), len(bases) * 4)

    def test_check_mode(self) -> None:
        text = live()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "exactness_gen.c"
            path.write_text(text, encoding="utf-8")
            self.assertEqual(gen.check(path, text), 0)
            path.write_text(text.replace("exact", "exacT", 1), encoding="utf-8")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(gen.check(path, text), 1)
            self.assertIn("exactness_gen.c", out.getvalue())
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gen.main(["--check", "--path", str(path)]), 1)
            self.assertEqual(gen.main(["--write", "--path", str(path)]), 0)
            self.assertEqual(gen.main(["--check", "--path", str(path)]), 0)


if __name__ == "__main__":
    unittest.main()
