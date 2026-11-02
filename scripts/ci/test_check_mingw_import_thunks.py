#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for ``scripts/ci/check_mingw_import_thunks.py`` (ADR-3397, #3257)."""

from __future__ import annotations

import importlib.util
import io
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_mingw_import_thunks",
    Path(__file__).with_name("check_mingw_import_thunks.py"),
)
assert _SPEC and _SPEC.loader
gate = importlib.util.module_from_spec(_SPEC)
sys.modules["check_mingw_import_thunks"] = gate
_SPEC.loader.exec_module(gate)

# Trimmed from x86_64-w64-mingw32-nm on b-nolto/src/libvmaf.dll.a (healthy import library).
WITH_THUNKS = """\
libvmaf_3_dll_d000078.o:
                 U _head_libvmaf_3_dll
0000000000000000 i .idata$4
0000000000000000 I __imp_vmaf_write_output_with_format
0000000000000000 t .text
0000000000000000 T vmaf_write_output_with_format

libvmaf_3_dll_d000042.o:
0000000000000000 I __imp_vmaf_picture_alloc
0000000000000000 t .text
0000000000000000 T vmaf_picture_alloc
"""

# Trimmed from x86_64-w64-mingw32-nm on b-lto/src/libvmaf.dll.a (broken LTO import library:
# __imp_ and __nm_ entries, zero T function thunks).
PLANTED_LTO_NO_THUNKS = """\
libvmaf_3_dll_d000078.o:
                 U _head_libvmaf_3_dll
0000000000000000 i .idata$4
0000000000000000 I __imp_vmaf_write_output_with_format
0000000000000000 I __nm_vmaf_write_output_with_format
0000000000000000 t .text

libvmaf_3_dll_d000042.o:
0000000000000000 I __imp_vmaf_picture_alloc
0000000000000000 I __nm_vmaf_picture_alloc
0000000000000000 t .text
"""

BROKEN_LTO_VMAFX_IMP = 1183
BROKEN_LTO_VMAFX_THUNKS = 48
BOUNDARY_IMP = 10
BOUNDARY_PASS_THUNKS = 5
BOUNDARY_FAIL_THUNKS = 4


def _generate_nm_output(imp_count: int, thunk_count: int) -> str:
    lines = [f"0000000000000000 I __imp_sym_{i}" for i in range(imp_count)]
    lines.extend([f"0000000000000000 T sym_{j}" for j in range(thunk_count)])
    return "\n".join(lines) + "\n"


def _make_build_dir(root: Path, names: tuple[str, ...]) -> Path:
    src = root / "src"
    src.mkdir(parents=True, exist_ok=True)
    for name in names:
        (src / name).write_bytes(b"!<arch>\n")
    return root


def test_positive_thunks_present_passes() -> None:
    """Positive: import libraries with both __imp_ and T thunks exit 0 and print per-file OK."""
    with tempfile.TemporaryDirectory() as tmp:
        build = _make_build_dir(Path(tmp), ("libvmaf.dll.a", "libvmafx.dll.a"))
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, WITH_THUNKS, ""),
            )
        assert rc == 0, err.getvalue()
        assert err.getvalue() == ""
        lines = out.getvalue().strip().splitlines()
        assert len(lines) == len(("libvmaf.dll.a", "libvmafx.dll.a"))
        assert "OK" in lines[0] and "libvmaf.dll.a: __imp_=2, T=2" in lines[0]
        assert "OK" in lines[1] and "libvmafx.dll.a: __imp_=2, T=2" in lines[1]


def test_negative_planted_only_imp_no_thunks_fails() -> None:
    """Negative (planted): __imp_ > 0 and T == 0 fails (exit 1) naming file and both counts."""
    with tempfile.TemporaryDirectory() as tmp:
        build = _make_build_dir(Path(tmp), ("libvmaf.dll.a", "libvmafx.dll.a"))
        outputs = {
            "libvmaf.dll.a": PLANTED_LTO_NO_THUNKS,
            "libvmafx.dll.a": WITH_THUNKS,
        }
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, path: (0, outputs[path.name], ""),
            )
        assert rc == 1
        assert "libvmaf.dll.a" in err.getvalue()
        assert "__imp_=2, T=0" in err.getvalue()
        assert "libvmafx.dll.a: __imp_=2, T=2" in out.getvalue()


def test_boundary_no_dll_a_found_fails() -> None:
    """Boundary: a build dir with no *.dll.a under src/ fails closed with exit 1."""
    with tempfile.TemporaryDirectory() as tmp:
        empty_build = Path(tmp) / "empty"
        (empty_build / "src").mkdir(parents=True)
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.main([str(empty_build)])
        assert rc == 1
        assert "no *.dll.a import libraries found" in err.getvalue()


def test_boundary_nm_failure_nonzero_fails() -> None:
    """Boundary: a non-zero exit status from nm fails closed with exit 1."""
    with tempfile.TemporaryDirectory() as tmp:
        build = _make_build_dir(Path(tmp), ("libvmaf.dll.a",))
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="fake-nm",
                runner=lambda _tool, _path: (2, "", "file format not recognized"),
            )
        assert rc == 1
        assert "libvmaf.dll.a" in err.getvalue()
        assert "exit 2" in err.getvalue()
        assert "file format not recognized" in err.getvalue()


def test_fake_nm_script_end_to_end() -> None:
    """End-to-end CLI invocation with a fake nm script on disk (0o500 permissions)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        build = _make_build_dir(tmp_path, ("libvmaf.dll.a",))
        fake_nm = tmp_path / "fake_nm.sh"
        fake_nm.write_text(
            f"#!/bin/sh\ncat <<'EOF'\n{PLANTED_LTO_NO_THUNKS}EOF\n",
            encoding="utf-8",
        )
        fake_nm.chmod(0o500)
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.main(["--nm", str(fake_nm), str(build)])
        assert rc == 1
        assert "libvmaf.dll.a: __imp_=2, T=0" in err.getvalue()


def test_negative_planted_broken_lto_ratio_fails() -> None:
    """Planted negative: libvmafx broken LTO build (imp=1183, T=48) fails closed."""
    with tempfile.TemporaryDirectory() as tmp:
        build = _make_build_dir(Path(tmp), ("libvmafx.dll.a",))
        nm_out = _generate_nm_output(BROKEN_LTO_VMAFX_IMP, BROKEN_LTO_VMAFX_THUNKS)
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, nm_out, ""),
            )
        assert rc == 1
        assert "libvmafx.dll.a" in err.getvalue()
        assert f"__imp_={BROKEN_LTO_VMAFX_IMP}, T={BROKEN_LTO_VMAFX_THUNKS}" in err.getvalue()


def test_boundary_thunk_ratio_at_threshold_passes() -> None:
    """Boundary: imp=10 and T=5 exactly meets MIN_THUNK_RATIO=0.5 and passes."""
    with tempfile.TemporaryDirectory() as tmp:
        build = _make_build_dir(Path(tmp), ("libvmaf.dll.a",))
        nm_out = _generate_nm_output(BOUNDARY_IMP, BOUNDARY_PASS_THUNKS)
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, nm_out, ""),
            )
        assert rc == 0, err.getvalue()
        assert f"libvmaf.dll.a: __imp_={BOUNDARY_IMP}, T={BOUNDARY_PASS_THUNKS}" in out.getvalue()


def test_boundary_thunk_ratio_below_threshold_fails() -> None:
    """Boundary: imp=10 and T=4 falls below MIN_THUNK_RATIO=0.5 and fails."""
    with tempfile.TemporaryDirectory() as tmp:
        build = _make_build_dir(Path(tmp), ("libvmaf.dll.a",))
        nm_out = _generate_nm_output(BOUNDARY_IMP, BOUNDARY_FAIL_THUNKS)
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, nm_out, ""),
            )
        assert rc == 1
        assert "libvmaf.dll.a" in err.getvalue()
        assert f"__imp_={BOUNDARY_IMP}, T={BOUNDARY_FAIL_THUNKS}" in err.getvalue()


def test_imp_zero_passes() -> None:
    """Boundary: an archive with imp=0 and T=0 exits 0 (no imports)."""
    with tempfile.TemporaryDirectory() as tmp:
        build = _make_build_dir(Path(tmp), ("libvmaf.dll.a",))
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, "", ""),
            )
        assert rc == 0, err.getvalue()
        assert "libvmaf.dll.a: __imp_=0, T=0" in out.getvalue()
