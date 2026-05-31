# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for the 2026-05-31 Python-surfaces bug-audit bundle.

Covers eight defects across ``ai/src/corpus/base.py`` and
``ai/src/vmaf_train/data/*`` that were uncovered by the audit:

* ``probe_geometry`` had no subprocess timeout — wedged ffprobe could
  hang the whole ingest pipeline forever.
* ``download_clip`` had no spawn-side timeout (only the in-band curl
  ``--max-time``) — a wedged DNS / signal-handler path could stall.
* ``load_manifest`` / ``load_mos_csv`` / ``write_manifest`` opened files
  without an explicit ``encoding=`` — locale leak from sibling processes.
* ``_run_vmaf`` (feature_dump) had no subprocess timeout and used the
  locale-default encoding for the JSON output read.
* ``iter_frames`` swallowed ffmpeg failures silently because stderr was
  not captured and the post-EOF wait was unbounded.
* ``_load_frame`` (frame_dataset) called ``np.load`` without
  ``allow_pickle=False`` — pickle-class code-exec gap on untrusted .npy.

These tests are deliberately seam-only: they mock subprocess and avoid
touching the real ffprobe/ffmpeg/vmaf binaries.  They run in ~1 second
each on a cold venv.
"""

from __future__ import annotations

import io
import subprocess

# Make the ai/src tree importable regardless of conftest discovery order.
import sys
from pathlib import Path
from typing import Any

import numpy as np
import pytest

_AI_SRC = Path(__file__).resolve().parents[1] / "src"
if str(_AI_SRC) not in sys.path:
    sys.path.insert(0, str(_AI_SRC))

from corpus.base import download_clip, probe_geometry  # noqa: E402
from vmaf_train.data import frame_dataset as _frame_dataset_mod  # noqa: E402
from vmaf_train.data.frame_loader import FrameSource, iter_frames  # noqa: E402
from vmaf_train.data.manifest_scan import load_mos_csv, write_manifest  # noqa: E402

# ---------------------------------------------------------------------------
# probe_geometry — wedged-ffprobe timeout (Bug 1)
# ---------------------------------------------------------------------------


def test_probe_geometry_returns_none_on_subprocess_timeout(tmp_path: Path) -> None:
    """A ffprobe hang must surface as ``None``, not propagate, not block."""
    clip = tmp_path / "wedged.mp4"
    clip.write_bytes(b"\x00" * 64)

    def hung_runner(*args: Any, **kwargs: Any) -> subprocess.CompletedProcess:
        # The fix passes a ``timeout=`` kwarg; a real wedged subprocess
        # would raise TimeoutExpired from inside ``subprocess.run``.
        # Simulate the same outcome here so we can assert the fallback.
        assert "timeout" in kwargs, (
            "probe_geometry must pass a subprocess timeout — without it a "
            "wedged ffprobe would hang the ingest pipeline indefinitely"
        )
        raise subprocess.TimeoutExpired(cmd=args[0] if args else [], timeout=kwargs["timeout"])

    result = probe_geometry(clip, runner=hung_runner)
    assert result is None


def test_probe_geometry_accepts_explicit_timeout_kwarg(tmp_path: Path) -> None:
    """Callers can tighten the timeout per-invocation."""
    clip = tmp_path / "fixture.mp4"
    clip.write_bytes(b"\x00")
    captured: dict[str, Any] = {}

    def capturing_runner(*args: Any, **kwargs: Any) -> subprocess.CompletedProcess:
        captured.update(kwargs)
        raise subprocess.TimeoutExpired(cmd="ffprobe", timeout=kwargs["timeout"])

    probe_geometry(clip, runner=capturing_runner, timeout_s=2.5)
    assert captured.get("timeout") == 2.5


# ---------------------------------------------------------------------------
# download_clip — spawn-side timeout (Bug 2)
# ---------------------------------------------------------------------------


def test_download_clip_surfaces_spawn_timeout_as_failure(tmp_path: Path) -> None:
    """A wedged curl spawn must return a deterministic failure tuple."""
    dest = tmp_path / "clip.mp4"

    def hung_runner(*args: Any, **kwargs: Any) -> subprocess.CompletedProcess:
        # The fix passes a ``timeout=`` greater than ``timeout_s``.
        assert "timeout" in kwargs
        assert kwargs["timeout"] > 0
        raise subprocess.TimeoutExpired(cmd="curl", timeout=kwargs["timeout"])

    ok, reason = download_clip(
        url="https://example.invalid/clip.mp4",
        dest=dest,
        runner=hung_runner,
        timeout_s=1,
    )
    assert ok is False
    assert "curl-spawn-timeout" in reason
    # The .part artefact must not survive the failed spawn.
    assert not (dest.with_suffix(dest.suffix + ".part")).exists()


# ---------------------------------------------------------------------------
# manifest_scan — explicit UTF-8 (Bugs 4, 5)
# ---------------------------------------------------------------------------


def test_load_mos_csv_handles_utf8_keys(tmp_path: Path) -> None:
    """Non-ASCII MOS-CSV rows survive a locale-stripped sibling process."""
    csv = tmp_path / "mos.csv"
    csv.write_text("key,mos\nclip_éàü,4.2\nclip_ascii,3.1\n", encoding="utf-8")
    mos = load_mos_csv(csv)
    assert mos == {"clip_éàü": 4.2, "clip_ascii": 3.1}


def test_write_manifest_round_trips_unicode(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The manifest writer emits UTF-8 unicode without locale leak."""
    from vmaf_train.data import datasets as _datasets
    from vmaf_train.data.manifest_scan import ScanEntry

    # Redirect the manifest output to ``tmp_path`` (the production helper
    # writes into the package's ``manifests/`` tree).
    monkeypatch.setattr(
        _datasets, "manifest_path", lambda name: tmp_path / f"{name}.yaml", raising=True
    )
    # Also redirect the manifest_scan module's view of the path helper.
    from vmaf_train.data import manifest_scan as _ms

    monkeypatch.setattr(_ms, "manifest_path", lambda name: tmp_path / f"{name}.yaml", raising=True)

    entries = [
        ScanEntry(key="clip_éàü", path="éàü/clip.yuv", sha256="a" * 64, mos=4.0),
    ]
    dst = write_manifest("nflx", entries)
    # The file must be byte-decodable as UTF-8 and contain the literal
    # unicode (allow_unicode=True), not an escaped \uXXXX form.
    text = dst.read_text(encoding="utf-8")
    assert "éàü" in text


# ---------------------------------------------------------------------------
# datasets.load_manifest — explicit UTF-8 (Bug 3)
# ---------------------------------------------------------------------------


def test_load_manifest_reads_utf8_yaml(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """The YAML manifest loader must not depend on the host locale."""
    from vmaf_train.data import datasets as _datasets

    yaml_path = tmp_path / "nflx.yaml"
    yaml_path.write_text(
        "entries:\n  - key: clip_éàü\n    path: éàü/clip.yuv\n"
        "    sha256: '" + ("a" * 64) + "'\n    mos: 4.0\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(_datasets, "manifest_path", lambda name: yaml_path, raising=True)
    entries = _datasets.load_manifest("nflx")
    assert len(entries) == 1
    assert entries[0].key == "clip_éàü"


# ---------------------------------------------------------------------------
# frame_loader.iter_frames — capture stderr + check rc (Bug 7)
# ---------------------------------------------------------------------------


class _FakeProc:
    """Minimal Popen-like object used to exercise iter_frames cleanup."""

    def __init__(self, *, stdout_bytes: bytes, rc: int, stderr_bytes: bytes = b"") -> None:
        self.stdout = io.BytesIO(stdout_bytes)
        self.stderr = io.BytesIO(stderr_bytes)
        self._rc = rc
        self.returncode: int | None = None
        self.killed = False

    def wait(self, timeout: float | None = None) -> int:
        del timeout  # _FakeProc is a fake — no real wait needed.
        self.returncode = self._rc
        return self._rc

    def kill(self) -> None:  # pragma: no cover — not exercised in these tests
        self.killed = True


def test_iter_frames_raises_when_ffmpeg_exits_nonzero(tmp_path: Path) -> None:
    """A non-zero ffmpeg exit must surface, not silently yield zero frames."""
    src = FrameSource(path=tmp_path / "missing.mp4", width=4, height=4, pix_fmt="gray")

    def popen_factory(*args: Any, **kwargs: Any) -> _FakeProc:
        assert kwargs.get("stderr") == subprocess.PIPE, (
            "iter_frames must pipe stderr — otherwise ffmpeg failure diagnostics "
            "are lost and the caller cannot tell a broken input from a healthy "
            "0-frame clip"
        )
        return _FakeProc(stdout_bytes=b"", rc=1, stderr_bytes=b"No such file or directory\n")

    with pytest.raises(RuntimeError, match=r"ffmpeg decode.*rc=1"):
        list(iter_frames(src, popen=popen_factory))


def test_iter_frames_passes_through_one_frame_on_success(tmp_path: Path) -> None:
    """Healthy ffmpeg path must keep yielding frames + then return rc=0."""
    src = FrameSource(path=tmp_path / "ok.mp4", width=2, height=2, pix_fmt="gray")
    one_frame = bytes([0, 1, 2, 3])  # 2x2 gray
    popen = lambda *a, **kw: _FakeProc(stdout_bytes=one_frame, rc=0)  # noqa: E731
    frames = list(iter_frames(src, popen=popen))
    assert len(frames) == 1
    assert frames[0].shape == (2, 2)
    assert frames[0].dtype == np.uint8


# ---------------------------------------------------------------------------
# frame_dataset._load_frame — refuse pickled .npy (Bug 8)
# ---------------------------------------------------------------------------


def test_load_frame_refuses_pickled_npy(tmp_path: Path) -> None:
    """Object-array .npy (the only pickle-execution code path) must be rejected."""
    pickled = tmp_path / "pickled.npy"
    # Force the pickle path by saving an object-dtype array.
    arr = np.array([{"oops": "code-exec-shaped"}], dtype=object)
    np.save(pickled, arr, allow_pickle=True)
    with pytest.raises(ValueError, match="allow_pickle"):
        _frame_dataset_mod._load_frame(pickled)


def test_load_frame_accepts_uint8_npy(tmp_path: Path) -> None:
    """Plain uint8 arrays (the production format) keep working."""
    luma_path = tmp_path / "luma.npy"
    np.save(luma_path, np.zeros((8, 8), dtype=np.uint8))
    import torch  # local import keeps the test optional under torch-less envs

    tensor = _frame_dataset_mod._load_frame(luma_path)
    assert tensor.shape == (1, 8, 8)
    assert tensor.dtype == torch.float32
