# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
from __future__ import annotations

import io
from pathlib import Path
from typing import Any, cast

import numpy as np
import pytest

from vmaf_train.data.frame_loader import FrameSource, iter_frames


class _FakeProcess:
    def __init__(self, payload: bytes) -> None:
        self.stdout = io.BytesIO(payload)
        # iter_frames now reads proc.stderr after the ffmpeg subprocess exits.
        # The fake process has no real stderr pipe; set to None so the
        # "if proc.stderr is not None" guard in iter_frames skips the read.
        self.stderr: io.BytesIO | None = None
        self.wait_called = False

    def wait(self, timeout: float | None = None) -> int:
        self.wait_called = True
        return 0


def _popen_factory(payload: bytes, captured: dict[str, Any]):
    def fake_popen(argv: list[str], *, stdout: int, stderr: int | None = None) -> _FakeProcess:
        captured["argv"] = argv
        captured["stdout"] = stdout
        captured["stderr"] = stderr
        proc = _FakeProcess(payload)
        captured["proc"] = proc
        return proc

    return fake_popen


def test_iter_frames_gray_keeps_2d_shape() -> None:
    payload = bytes([0, 1, 2, 3, 4, 5])
    captured: dict[str, Any] = {}
    source = FrameSource(path=Path("clip.mp4"), width=3, height=2, pix_fmt="gray")

    frames = list(
        iter_frames(source, ffmpeg="ffmpeg-test", popen=_popen_factory(payload, captured))
    )

    assert len(frames) == 1
    assert frames[0].shape == (2, 3)
    np.testing.assert_array_equal(frames[0], np.array([[0, 1, 2], [3, 4, 5]], dtype=np.uint8))
    assert captured["argv"] == [
        "ffmpeg-test",
        "-v",
        "error",
        "-i",
        "clip.mp4",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "gray",
        "-",
    ]
    assert captured["stdout"] == -1
    assert cast(_FakeProcess, captured["proc"]).wait_called is True


@pytest.mark.parametrize(
    ("pix_fmt", "channels"),
    [
        ("rgb24", 3),
        ("bgr24", 3),
        ("rgba", 4),
        ("bgra", 4),
    ],
)
def test_iter_frames_packed_color_keeps_channel_axis(pix_fmt: str, channels: int) -> None:
    frame_bytes = 2 * 2 * channels
    payload = bytes(range(frame_bytes)) + b"partial"
    captured: dict[str, Any] = {}
    source = FrameSource(path=Path("clip.mov"), width=2, height=2, pix_fmt=pix_fmt)

    frames = list(iter_frames(source, popen=_popen_factory(payload, captured)))

    assert len(frames) == 1
    assert frames[0].shape == (2, 2, channels)
    np.testing.assert_array_equal(
        frames[0],
        np.arange(frame_bytes, dtype=np.uint8).reshape(2, 2, channels),
    )
    argv = cast(list[str], captured["argv"])
    assert "-pix_fmt" in argv
    assert argv[argv.index("-pix_fmt") + 1] == pix_fmt
    assert cast(_FakeProcess, captured["proc"]).wait_called is True


def test_iter_frames_rejects_unsupported_pix_fmt_before_spawning() -> None:
    def should_not_run(*_args, **_kwargs):
        raise AssertionError("ffmpeg should not be spawned")

    source = FrameSource(path=Path("clip.mov"), width=2, height=2, pix_fmt="yuv420p")
    with pytest.raises(ValueError, match="unsupported pix_fmt"):
        list(iter_frames(source, popen=should_not_run))
