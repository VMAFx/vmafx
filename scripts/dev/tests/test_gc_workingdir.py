# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Positive, negative and boundary controls for the working-directory GC."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "gc_workingdir", ROOT / "scripts/dev/gc_workingdir.py"
)
assert SPEC is not None and SPEC.loader is not None
GC = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GC)


class GcWorkingdirTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = TemporaryDirectory()
        self.repo = Path(self.tmp.name)
        self.state = self.repo / ".workingdir"
        run = self.state / "cache" / "gate-20260908"
        (run / "go-cache" / "49").mkdir(parents=True)
        (run / "go-cache" / "49" / "blob-d").write_bytes(b"x" * 4096)
        (run / "build" / "libvmaf.so.1").parent.mkdir(parents=True, exist_ok=True)
        (run / "build" / "libvmaf.so.1").write_bytes(b"y" * 2048)
        (run / "objects").mkdir()
        (run / "objects" / "motion.o").write_bytes(b"z" * 1024)
        (run / "receipt.json").write_text("{}", encoding="utf-8")
        (run / "clang-tidy-help.txt").write_text("help", encoding="utf-8")
        self.run = run
        self.addCleanup(self.tmp.cleanup)

    def victims(self, citations: set[str] | None = None) -> set[str]:
        found = GC.collect(self.state, citations or set())
        return {path.relative_to(self.state).as_posix() for path, _, _ in found}

    # positive
    def test_rebuildable_output_is_collected(self) -> None:
        victims = self.victims()
        self.assertIn("cache/gate-20260908/go-cache", victims)
        self.assertIn("cache/gate-20260908/objects/motion.o", victims)
        self.assertIn("cache/gate-20260908/build", victims)

    def test_evidence_is_never_collected(self) -> None:
        victims = self.victims()
        self.assertNotIn("cache/gate-20260908/receipt.json", victims)
        self.assertNotIn("cache/gate-20260908/clang-tidy-help.txt", victims)

    def test_versioned_shared_object_matches(self) -> None:
        loose = self.state / "cache" / "gate-20260908" / "libfoo.so.1.2.3"
        loose.write_bytes(b"s")
        self.assertIn("cache/gate-20260908/libfoo.so.1.2.3", self.victims())

    def test_build_prefixed_directories_match(self) -> None:
        for name in ("build-arm64", "build_cpu"):
            (self.state / "cache" / "gate-20260908" / name).mkdir()
        victims = self.victims()
        self.assertIn("cache/gate-20260908/build-arm64", victims)
        self.assertIn("cache/gate-20260908/build_cpu", victims)

    # negative
    def test_a_citation_protects_the_exact_path(self) -> None:
        cited = {"cache/gate-20260908/go-cache"}
        self.assertNotIn("cache/gate-20260908/go-cache", self.victims(cited))

    def test_a_citation_does_not_protect_paths_below_it(self) -> None:
        # Citing the run directory keeps the directory, not its object tree.
        cited = {"cache/gate-20260908"}
        self.assertIn("cache/gate-20260908/go-cache", self.victims(cited))

    def test_protected_roots_are_untouched(self) -> None:
        for name in GC.PROTECTED_NAMES:
            objects = self.state / name / "build"
            objects.mkdir(parents=True)
            (objects / "a.o").write_bytes(b"a")
        victims = self.victims()
        self.assertEqual([v for v in victims if v.split("/")[0] in GC.PROTECTED_NAMES], [])

    def test_symlinks_are_skipped(self) -> None:
        link = self.state / "cache" / "gate-20260908" / "alias.o"
        link.symlink_to(self.run / "objects" / "motion.o")
        self.assertNotIn("cache/gate-20260908/alias.o", self.victims())

    def test_refuses_a_state_tree_outside_the_repository(self) -> None:
        with TemporaryDirectory() as outside:
            code = GC.main([outside, "--repo-root", str(self.repo), "--no-citations"])
        self.assertEqual(code, 65)

    def test_missing_state_tree_is_reported(self) -> None:
        code = GC.main([".absent", "--repo-root", str(self.repo), "--no-citations"])
        self.assertEqual(code, 66)

    # boundary
    def test_apply_removes_and_records_a_manifest(self) -> None:
        code = GC.main(
            [
                ".workingdir",
                "--repo-root",
                str(self.repo),
                "--no-citations",
                "--apply",
                "--top",
                "0",
            ]
        )
        self.assertEqual(code, 0)
        self.assertFalse((self.run / "go-cache").exists())
        self.assertFalse((self.run / "objects" / "motion.o").exists())
        self.assertTrue((self.run / "receipt.json").exists())
        manifest = self.run / GC.MANIFEST
        self.assertTrue(manifest.is_file())
        body = manifest.read_text(encoding="utf-8")
        self.assertIn("Reclaimed", body)
        self.assertIn("go-cache", body)

    def test_a_second_apply_is_a_no_op(self) -> None:
        args = [
            ".workingdir",
            "--repo-root",
            str(self.repo),
            "--no-citations",
            "--apply",
            "--top",
            "0",
        ]
        self.assertEqual(GC.main(args), 0)
        self.assertEqual(GC.main(args), 0)

    def test_human_readable_sizes_at_the_unit_boundary(self) -> None:
        self.assertEqual(GC.human(0), "0.0 B")
        self.assertEqual(GC.human(GC.KIB - 1), "1023.0 B")
        self.assertEqual(GC.human(GC.KIB), "1.0 KiB")
        self.assertEqual(GC.human(GC.KIB**4), "1.0 TiB")
        self.assertTrue(GC.human(GC.KIB**6).endswith("TiB"))


if __name__ == "__main__":
    unittest.main()
