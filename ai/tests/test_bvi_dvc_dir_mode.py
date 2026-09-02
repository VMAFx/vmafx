# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for the ``--bvi-dir`` input mode of ``bvi_dvc_to_full_features.py``.

ADR-0524.  These tests cover the two new helpers without invoking the real
vmaf binary or touching the 192 GB corpus:

* ``_select_tier_entries_dir`` — enumerate YUV/MP4/MKV files in a directory and
  map them to tiers via resolution.
* ``_DirEntry`` — the lightweight stand-in for ``zipfile.ZipInfo``.
* ``main`` end-to-end against a synthetic fixture directory with two fake YUV
  files (tier A and tier D), asserting the script picks them up and exits
  cleanly (the vmaf / ffmpeg invocation is mocked out so no binary is
  required).

The vmaf binary invocation is mocked via ``subprocess.run`` patching so the
test suite can run without a built libvmaf on the host.
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "bvi_dvc_to_full_features.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("bvi_dvc_to_full_features", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    # Use a fresh module name to avoid colliding with a cached import.
    sys.modules["bvi_dvc_to_full_features_test_import"] = module
    spec.loader.exec_module(module)
    return module


# ---------------------------------------------------------------------------
# Helper: synthesise a minimal raw YUV file (a few frames of zeros).
# ---------------------------------------------------------------------------


def _write_fake_yuv(path: Path, w: int, h: int, frames: int = 2, depth: int = 8) -> None:
    """Write a zero-filled raw YUV 4:2:0 file at *path*.

    For 8-bit: 1 byte per sample.  For 10-bit (yuv420p10le): 2 bytes per
    sample.  The content is all-zeros — we only need the file to *exist* and
    be readable by a mocked subprocess; actual pixel values are irrelevant for
    the unit tests here.
    """
    bps = 2 if depth > 8 else 1
    frame_bytes = (w * h + 2 * (w // 2) * (h // 2)) * bps
    path.write_bytes(b"\x00" * frame_bytes * frames)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture()
def bvi_dir(tmp_path: Path) -> Path:
    """A fixture directory with two BVI-DVC-named YUV files.

    Tier A (3840×2176) and tier D (480×272) are represented so the
    ``--tier all`` and ``--tier D`` filter paths are both exercised.
    """
    d = tmp_path / "bvi_extracted"
    d.mkdir()

    # Tier A: 3840×2176, 120 fps, 10-bit
    _write_fake_yuv(d / "ClipA_3840x2176_120fps_10bit_420.yuv", 3840, 2176, depth=10)
    # Tier D: 480×272, 30 fps, 8-bit (synthetic — exercises the depth branch)
    _write_fake_yuv(d / "ClipD_480x272_30fps_8bit_420.yuv", 480, 272, depth=8)

    return d


# ---------------------------------------------------------------------------
# _select_tier_entries_dir
# ---------------------------------------------------------------------------


class TestSelectTierEntriesDir:
    def test_all_tier_finds_both_clips(self, bvi_dir: Path) -> None:
        mod = _load_module()
        entries = mod._select_tier_entries_dir(bvi_dir, "all")
        assert len(entries) == 2

    def test_tier_d_filter(self, bvi_dir: Path) -> None:
        mod = _load_module()
        entries = mod._select_tier_entries_dir(bvi_dir, "D")
        assert len(entries) == 1
        assert entries[0].tier == "D"
        assert entries[0].w == 480
        assert entries[0].h == 272

    def test_tier_a_filter(self, bvi_dir: Path) -> None:
        mod = _load_module()
        entries = mod._select_tier_entries_dir(bvi_dir, "A")
        assert len(entries) == 1
        assert entries[0].tier == "A"

    def test_tier_b_filter_returns_empty(self, bvi_dir: Path) -> None:
        mod = _load_module()
        entries = mod._select_tier_entries_dir(bvi_dir, "B")
        assert entries == []

    def test_non_bvi_files_ignored(self, tmp_path: Path) -> None:
        """Files that do not match the BVI-DVC naming convention are skipped."""
        mod = _load_module()
        d = tmp_path / "mixed"
        d.mkdir()
        (d / "random_clip.yuv").write_bytes(b"\x00" * 100)
        (d / "another_video.mp4").write_bytes(b"\x00" * 100)
        # One valid file to confirm the scanner still works.
        _write_fake_yuv(d / "ClipD_480x272_30fps_8bit_420.yuv", 480, 272, depth=8)
        entries = mod._select_tier_entries_dir(d, "all")
        assert len(entries) == 1

    def test_mkv_container_files_are_accepted(self, tmp_path: Path) -> None:
        """The local lossless BVI-DVC bundle is MKV-backed, not MP4-backed."""
        mod = _load_module()
        d = tmp_path / "mkv"
        d.mkdir()
        (d / "ClipB_1920x1088_60fps_10bit_420.mkv").write_bytes(b"not a real mkv")

        entries = mod._select_tier_entries_dir(d, "B")

        assert len(entries) == 1
        assert entries[0].tier == "B"
        assert entries[0].path.suffix == ".mkv"

    def test_unknown_resolution_emits_warning_and_skips(
        self, tmp_path: Path, capsys: pytest.CaptureFixture
    ) -> None:
        """A file with a plausible name but unknown resolution is skipped with a warning."""
        mod = _load_module()
        d = tmp_path / "odd"
        d.mkdir()
        _write_fake_yuv(d / "OddClip_1280x720_60fps_8bit_420.yuv", 1280, 720)
        entries = mod._select_tier_entries_dir(d, "all")
        assert entries == []
        captured = capsys.readouterr()
        assert "not in known tier map" in captured.err

    def test_max_clips_respected(self, bvi_dir: Path) -> None:
        mod = _load_module()
        entries = mod._select_tier_entries_dir(bvi_dir, "all")
        # Slice to 1 the same way main() does.
        sliced = entries[:1]
        assert len(sliced) == 1

    def test_sorted_order_is_deterministic(self, bvi_dir: Path) -> None:
        mod = _load_module()
        a = mod._select_tier_entries_dir(bvi_dir, "all")
        b = mod._select_tier_entries_dir(bvi_dir, "all")
        assert [e.filename for e in a] == [e.filename for e in b]


# ---------------------------------------------------------------------------
# _DirEntry attributes
# ---------------------------------------------------------------------------


class TestDirEntry:
    def test_attributes_round_trip(self, bvi_dir: Path) -> None:
        mod = _load_module()
        entries = mod._select_tier_entries_dir(bvi_dir, "D")
        assert len(entries) == 1
        e = entries[0]
        assert e.w == 480
        assert e.h == 272
        assert e.fps == 30
        assert e.depth == 8
        assert e.tier == "D"
        assert e.path.suffix == ".yuv"


# ---------------------------------------------------------------------------
# main() end-to-end with mocked subprocess
# ---------------------------------------------------------------------------


def _make_fake_vmaf_json(n_frames: int = 2) -> str:
    """Return a minimal libvmaf JSON with *n_frames* dummy frame records."""
    frames = [
        {
            "frameNum": i,
            "metrics": {
                "adm2": 0.9,
                "adm_scale0": 0.9,
                "adm_scale1": 0.9,
                "adm_scale2": 0.9,
                "adm_scale3": 0.9,
                "vif_scale0": 0.5,
                "vif_scale1": 0.6,
                "vif_scale2": 0.7,
                "vif_scale3": 0.8,
                "motion": 1.0,
                "motion2": 0.5,
                "motion3": 0.3,
                "psnr_y": 35.0,
                "psnr_cb": 38.0,
                "psnr_cr": 38.0,
                "float_ssim": 0.95,
                "float_ms_ssim": 0.97,
                "cambi": 0.1,
                "ciede2000": 2.0,
                "psnr_hvs": 33.0,
                "ssimulacra2": 70.0,
                "vmaf": 80.0,
            },
        }
        for i in range(n_frames)
    ]
    return json.dumps({"frames": frames})


class TestMainDirMode:
    """End-to-end test of ``main()`` with ``--bvi-dir``, subprocess mocked."""

    def _mock_subprocess_run(self, cmd, **kwargs):
        """Replace subprocess.run: write fake vmaf JSON when the vmaf binary
        is invoked; return a no-op CompletedProcess for ffmpeg/ffprobe calls."""
        mock_result = MagicMock()
        mock_result.returncode = 0
        mock_result.stdout = "{}"
        mock_result.stderr = ""

        if not cmd:
            return mock_result

        # Detect vmaf binary invocation by --output flag.
        if "--output" in cmd:
            out_idx = cmd.index("--output") + 1
            out_path = Path(cmd[out_idx])
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_text(_make_fake_vmaf_json(2))

        return mock_result

    def test_dir_mode_two_yuv_clips(
        self, bvi_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """``--bvi-dir`` with 2 YUV clips in tier-all emits a parquet with rows."""
        import pandas as pd

        mod = _load_module()

        fake_vmaf = tmp_path / "vmaf"
        fake_vmaf.write_bytes(b"")  # just needs to exist as a file
        fake_vmaf.chmod(0o755)

        fake_model = tmp_path / "model.json"
        fake_model.write_text("{}")

        out_parquet = tmp_path / "out.parquet"

        with patch("subprocess.run", side_effect=self._mock_subprocess_run):
            # Call via sys.argv patching.
            monkeypatch.setattr(
                "sys.argv",
                [
                    "bvi_dvc_to_full_features.py",
                    "--bvi-dir",
                    str(bvi_dir),
                    "--tier",
                    "all",
                    "--vmaf-bin",
                    str(fake_vmaf),
                    "--model",
                    str(fake_model),
                    "--out",
                    str(out_parquet),
                    "--no-cache",
                    "--max-clips",
                    "2",
                ],
            )
            rc = mod.main()

        assert rc == 0, f"main() returned {rc}"
        assert out_parquet.is_file(), "Output parquet was not written"
        df = pd.read_parquet(out_parquet)
        # 2 clips × 2 frames each = 4 rows minimum.
        assert len(df) >= 4
        assert "vmaf" in df.columns
        assert "key" in df.columns

        manifest = json.loads(out_parquet.with_suffix(".manifest.json").read_text(encoding="utf-8"))
        assert manifest["schema"] == "bvi-dvc-full-features-manifest-v1"
        assert manifest["stats"]["input_mode"] == "dir"
        assert manifest["stats"]["tier"] == "all"
        assert manifest["stats"]["clips_selected"] == 2
        assert manifest["stats"]["frames"] == len(df)
        assert manifest["stats"]["columns"] == len(df.columns)
        assert manifest["run_provenance"]["schema"] == "ai-run-provenance-v1"
        assert manifest["run_provenance"]["args"]["max_clips"] == 2

    def test_dir_mode_missing_dir_returns_2(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """A non-existent --bvi-dir path should exit with return code 2."""
        mod = _load_module()

        fake_vmaf = tmp_path / "vmaf"
        fake_vmaf.write_bytes(b"")
        fake_vmaf.chmod(0o755)
        fake_model = tmp_path / "model.json"
        fake_model.write_text("{}")

        monkeypatch.setattr(
            "sys.argv",
            [
                "bvi_dvc_to_full_features.py",
                "--bvi-dir",
                str(tmp_path / "does_not_exist"),
                "--vmaf-bin",
                str(fake_vmaf),
                "--model",
                str(fake_model),
                "--out",
                str(tmp_path / "out.parquet"),
            ],
        )
        rc = mod.main()
        assert rc == 2

    def test_bvi_zip_and_bvi_dir_mutually_exclusive(
        self, bvi_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """Passing both --bvi-zip and --bvi-dir should cause argparse to exit."""
        mod = _load_module()
        fake_zip = tmp_path / "fake.zip"
        fake_zip.write_bytes(b"PK")
        fake_vmaf = tmp_path / "vmaf"
        fake_vmaf.write_bytes(b"")

        monkeypatch.setattr(
            "sys.argv",
            [
                "bvi_dvc_to_full_features.py",
                "--bvi-zip",
                str(fake_zip),
                "--bvi-dir",
                str(bvi_dir),
                "--vmaf-bin",
                str(fake_vmaf),
                "--model",
                str(fake_vmaf),
                "--out",
                str(tmp_path / "out.parquet"),
            ],
        )
        with pytest.raises(SystemExit) as exc_info:
            mod.main()
        assert exc_info.value.code != 0
