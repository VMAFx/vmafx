# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Tests for the install lookup and scoring graph of scripts/ci/upstream-consumer-lib.sh.

The consumer scripts find the library under test through the pkg-config and
library directories of a prefix. Meson installs into lib, lib64 or, on Debian
and Ubuntu, the multiarch lib/<triplet>; the hosted runner uses the last one.
Positive (each layout), negative (no libvmaf.pc) and boundary (no library
directory at all) cases.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

LIB = Path(__file__).resolve().parent.parent / "upstream-consumer-lib.sh"
BASH = shutil.which("bash")
TIMEOUT = 30  # seconds per shell run (HISS-02)


def _call(function: str, prefix: Path) -> subprocess.CompletedProcess[str]:
    """Source the library and call one of its functions on `prefix`."""
    assert BASH is not None
    script = f'. "{LIB}" && {function} "$1"'
    # A fixed script over a path this test created; no user input.
    return subprocess.run(  # noqa: S603
        [BASH, "-c", script, "uc-test", str(prefix)],
        capture_output=True,
        text=True,
        check=False,
        timeout=TIMEOUT,
    )


@unittest.skipIf(BASH is None, "bash not found")
class InstallLookupTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.prefix = Path(self._tmp.name) / "prefix"

    def _install(self, libdir: str) -> Path:
        pc = self.prefix / libdir / "pkgconfig" / "libvmaf.pc"
        pc.parent.mkdir(parents=True)
        pc.write_text("Name: libvmaf\n", encoding="utf-8")
        return self.prefix / libdir

    def test_multiarch_install_is_found(self) -> None:
        libdir = self._install("lib/x86_64-linux-gnu")
        self.assertEqual(_call("uc_has_pc", self.prefix).returncode, 0)
        self.assertIn(f"{libdir}/pkgconfig", _call("uc_pkgpath", self.prefix).stdout.split(":"))
        self.assertIn(str(libdir), _call("uc_ldpath", self.prefix).stdout.strip().split(":"))

    def test_lib_and_lib64_installs_are_found(self) -> None:
        for libdir in ("lib", "lib64"):
            with self.subTest(libdir=libdir):
                shutil.rmtree(self.prefix, ignore_errors=True)
                found = self._install(libdir)
                self.assertEqual(_call("uc_has_pc", self.prefix).returncode, 0)
                paths = _call("uc_pkgpath", self.prefix).stdout.strip().split(":")
                self.assertIn(f"{found}/pkgconfig", paths)

    def test_prefix_without_libvmaf_pc_is_refused(self) -> None:
        (self.prefix / "lib" / "x86_64-linux-gnu" / "pkgconfig").mkdir(parents=True)
        self.assertNotEqual(_call("uc_has_pc", self.prefix).returncode, 0)

    def test_empty_prefix_keeps_well_formed_paths(self) -> None:
        self.prefix.mkdir()
        self.assertNotEqual(_call("uc_has_pc", self.prefix).returncode, 0)
        self.assertEqual(
            _call("uc_ldpath", self.prefix).stdout.strip(),
            f"{self.prefix}/lib:{self.prefix}/lib64",
        )
        self.assertTrue(
            _call("uc_pkgpath", self.prefix)
            .stdout.strip()
            .endswith(f"{self.prefix}/share/pkgconfig")
        )


def _graph(frames: str, out: str) -> subprocess.CompletedProcess[str]:
    """uc_ffmpeg_graph OUT with FRAMES set as a consumer script sets it."""
    assert BASH is not None
    script = f'. "{LIB}" && uc_defaults && FRAMES="$1" && uc_ffmpeg_graph "$2"'
    # A fixed script over arguments this test chose; no user input.
    return subprocess.run(  # noqa: S603
        [BASH, "-c", script, "uc-test", frames, out],
        capture_output=True,
        text=True,
        check=False,
        timeout=TIMEOUT,
    )


@unittest.skipIf(BASH is None, "bash not found")
class FFmpegGraphTests(unittest.TestCase):
    """The scoring graph shared by upstream-ffmpeg-compat.sh and the patch smoke."""

    def test_graph_cuts_both_inputs_before_libvmaf(self) -> None:
        graph = _graph("3", "ffmpeg.json").stdout.strip()
        cut = "trim=end_frame=3,setpts=PTS-STARTPTS"
        self.assertEqual(
            graph,
            f"[0:v]{cut}[d];[1:v]{cut}[r];"
            "[d][r]libvmaf=log_fmt=json:log_path=ffmpeg.json:n_threads=1",
        )

    def test_one_frame_boundary(self) -> None:
        graph = _graph("1", "out.json").stdout
        self.assertEqual(graph.count("trim=end_frame=1,"), 2)
        self.assertNotIn("end_frame=3", graph)

    def test_distorted_input_feeds_the_main_pad(self) -> None:
        graph = _graph("3", "x.json").stdout
        self.assertLess(graph.index("[0:v]"), graph.index("[1:v]"))
        self.assertIn("[d][r]libvmaf=", graph)
        self.assertNotIn("[r][d]libvmaf=", graph)


if __name__ == "__main__":
    unittest.main()
