# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/ci/check_msvc_library_names.py on built prefixes, good and bad."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "check_msvc_library_names", ROOT / "scripts/ci/check_msvc_library_names.py"
)
assert SPEC and SPEC.loader
MOD = importlib.util.module_from_spec(SPEC)
sys.modules["check_msvc_library_names"] = MOD
SPEC.loader.exec_module(MOD)

GOOD_PC = {
    "libvmaf.pc": "Name: libvmaf\nRequires: libvmafx\nLibs: -L${libdir} -lvmaf\n",
    "libvmafx.pc": "Name: libvmafx\nLibs: -L${libdir} -lvmafx\n",
}


def make_prefix(root: Path, libs: tuple[str, ...], pcs: dict[str, str]) -> Path:
    lib = root / "lib"
    (lib / "pkgconfig").mkdir(parents=True)
    for name in libs:
        (lib / name).write_bytes(b"!<arch>\n")
    for name, text in pcs.items():
        (lib / "pkgconfig" / name).write_text(text, encoding="utf-8")
    return root


class CheckMsvcLibraryNames(unittest.TestCase):
    def check(self, libs: tuple[str, ...], pcs: dict[str, str]) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            found: list[str] = MOD.problems(make_prefix(Path(tmp), libs, pcs))
        return found

    def test_static_msvc_install_passes(self) -> None:
        self.assertEqual(self.check(("vmaf.lib", "vmafx.lib"), GOOD_PC), [])

    def test_classic_names_fail(self) -> None:
        found = self.check(("libvmaf.a", "libvmafx.a"), GOOD_PC)
        self.assertEqual(len([p for p in found if p.endswith("missing")]), 2)
        self.assertEqual(len([p for p in found if "classic name" in p]), 2)

    def test_a_classic_name_beside_the_platform_name_fails(self) -> None:
        found = self.check(("vmaf.lib", "vmafx.lib", "libvmafx.a"), GOOD_PC)
        self.assertEqual(len(found), 1)
        self.assertIn("libvmafx.a", found[0])

    def test_pkg_config_without_the_link_flag_fails(self) -> None:
        pcs = dict(
            GOOD_PC, **{"libvmaf.pc": "Name: libvmaf\nRequires: libvmafx\nLibs: -L${libdir}\n"}
        )
        found = self.check(("vmaf.lib", "vmafx.lib"), pcs)
        self.assertEqual(len(found), 1)
        self.assertIn("libvmaf.pc", found[0])

    def test_lvmafx_does_not_satisfy_lvmaf(self) -> None:
        pcs = dict(GOOD_PC, **{"libvmaf.pc": "Requires: libvmafx\nLibs: -L${libdir} -lvmafx\n"})
        self.assertEqual(len(self.check(("vmaf.lib", "vmafx.lib"), pcs)), 1)

    def test_missing_pkg_config_file_fails(self) -> None:
        found = self.check(("vmaf.lib", "vmafx.lib"), {"libvmaf.pc": GOOD_PC["libvmaf.pc"]})
        self.assertEqual(found, [found[0]])
        self.assertIn("libvmafx.pc: missing", found[0])

    def test_main_exit_codes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            good = make_prefix(Path(tmp) / "good", ("vmaf.lib", "vmafx.lib"), GOOD_PC)
            bad = make_prefix(Path(tmp) / "bad", ("libvmaf.a",), GOOD_PC)
            self.assertEqual(MOD.main(["--prefix", str(good)]), 0)
            self.assertEqual(MOD.main(["--prefix", str(bad)]), 1)

    def test_the_configure_log_must_hold_no_name_warning(self) -> None:
        # The warning Meson's pkg-config module printed for vmaf.lib / vmafx.lib on the FFmpeg
        # Windows MSVC job (run 37902467527) before the names reached it as flags (ADR-2828).
        warning = (
            "WARNING: Library target 'vmafx' has 'name_suffix' set. Compilers may not find it "
            "from its '-lvmafx' linker flag in the 'libvmafx.pc' pkg-config file.\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            good = make_prefix(Path(tmp) / "good", ("vmaf.lib", "vmafx.lib"), GOOD_PC)
            clean = Path(tmp) / "clean.txt"
            clean.write_text("Build targets in project: 300\n", encoding="utf-8")
            noisy = Path(tmp) / "noisy.txt"
            noisy.write_text(warning, encoding="utf-8")
            self.assertEqual(MOD.main(["--prefix", str(good), "--meson-log", str(clean)]), 0)
            self.assertEqual(MOD.main(["--prefix", str(good), "--meson-log", str(noisy)]), 1)
            self.assertEqual(len(MOD.log_problems(noisy)), 1)
            missing = Path(tmp) / "absent.txt"
            self.assertEqual(MOD.main(["--prefix", str(good), "--meson-log", str(missing)]), 1)


if __name__ == "__main__":
    unittest.main()
