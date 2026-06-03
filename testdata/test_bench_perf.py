#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for the portable ``bench_perf.py`` harness."""

from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location("bench_perf", _HERE / "bench_perf.py")
assert _SPEC is not None
bench_perf = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
_SPEC.loader.exec_module(bench_perf)


def _touch(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x00")


def test_build_tests_keeps_mp4_decode_fixture_opt_in(tmp_path: Path) -> None:
    tests = bench_perf.build_tests(tmp_path)

    assert [test["name"] for test in tests] == [
        "BBB 1080p 48f (YUV)",
        "BBB 4K 200f (YUV)",
    ]

    with_mp4 = bench_perf.build_tests(tmp_path, bbb_mp4_ref=tmp_path / "bbb.mp4")
    assert with_mp4[-1]["name"] == "BBB 4K MP4 500f (decode+vmaf)"
    assert with_mp4[-1]["raw"] is False


def test_available_tests_skips_optional_missing_fixtures(tmp_path: Path) -> None:
    tests = bench_perf.build_tests(tmp_path)
    _touch(tmp_path / "bbb" / "ref_3840x2160_200f.yuv")
    _touch(tmp_path / "bbb" / "dis_3840x2160_200f.yuv")

    available = bench_perf.available_tests(tests, require_all=False)

    assert [test["name"] for test in available] == ["BBB 4K 200f (YUV)"]


def test_available_tests_can_require_every_configured_fixture(tmp_path: Path) -> None:
    tests = bench_perf.build_tests(tmp_path)

    with pytest.raises(FileNotFoundError, match="BBB 1080p 48f"):
        bench_perf.available_tests(tests, require_all=True)


def test_build_ffmpeg_command_uses_configured_binary_and_raw_inputs(tmp_path: Path) -> None:
    test = bench_perf.build_tests(tmp_path)[1]
    backend = bench_perf.backend_definitions(sycl_device="/dev/dri/renderD128")[0]

    cmd = bench_perf.build_ffmpeg_command(
        test,
        backend,
        log_path=tmp_path / "log.json",
        ffmpeg=Path("/opt/ffmpeg/bin/ffmpeg"),
    )

    assert cmd[0] == "/opt/ffmpeg/bin/ffmpeg"
    assert "-f" in cmd
    assert "rawvideo" in cmd
    assert str(tmp_path / "bbb" / "dis_3840x2160_200f.yuv") in cmd
    assert "libvmaf=log_path=" in " ".join(cmd)


def test_select_backends_rejects_unknown_backend() -> None:
    backends = bench_perf.backend_definitions(sycl_device="/dev/dri/renderD128")

    with pytest.raises(ValueError, match="unknown backend"):
        bench_perf.select_backends(backends, ("cpu", "bogus"))


def test_parse_args_backend_override_does_not_keep_default_all() -> None:
    args = bench_perf.parse_args(["--backend", "cpu"])

    assert args.backend == ["cpu"]
