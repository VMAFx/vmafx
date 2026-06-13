# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""ffmpeg-decoded frame batches for NR / learned-filter training (C2, C3)."""

from __future__ import annotations

import logging
import subprocess
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Protocol

import numpy as np

_LOG = logging.getLogger(__name__)

#: Wall-clock cap (seconds) for the post-EOF ``wait()`` on the ffmpeg child
#: process. The decoder should exit immediately after stdout is closed; a
#: stuck wait is a sign of a runaway process the caller should be told about
#: rather than blocking the generator's ``finally`` cleanup indefinitely.
_FFMPEG_WAIT_TIMEOUT_S: float = 30.0


@dataclass(frozen=True)
class FrameSource:
    path: Path
    width: int
    height: int
    pix_fmt: str = "gray"


class _PopenLike(Protocol):
    stdout: BinaryIO
    stderr: BinaryIO | None
    returncode: int | None

    def wait(self, timeout: float | None = ...) -> int: ...

    def kill(self) -> None: ...


_PIX_FMT_CHANNELS: dict[str, int] = {
    "gray": 1,
    "rgb24": 3,
    "bgr24": 3,
    "rgba": 4,
    "bgra": 4,
}


def _frame_shape(source: FrameSource) -> tuple[int, ...]:
    channels = _PIX_FMT_CHANNELS.get(source.pix_fmt)
    if channels is None:
        supported = ", ".join(sorted(_PIX_FMT_CHANNELS))
        raise ValueError(f"unsupported pix_fmt={source.pix_fmt!r}; expected one of {supported}")
    if channels == 1:
        return (source.height, source.width)
    return (source.height, source.width, channels)


def iter_frames(
    source: FrameSource,
    ffmpeg: str = "ffmpeg",
    popen=subprocess.Popen,
) -> Iterator[np.ndarray]:
    """Yield ffmpeg-decoded frames as uint8 numpy arrays.

    ``gray`` yields ``HxW`` arrays. Packed colour formats
    (``rgb24``, ``bgr24``, ``rgba``, ``bgra``) yield ``HxWxC`` arrays.

    Raises :class:`RuntimeError` if the ffmpeg subprocess exits non-zero
    (e.g. the source path is missing or the codec is unsupported). Without
    this check the generator would silently yield zero frames and the
    caller would treat the empty iteration as a healthy clip with no
    frames, which masks broken inputs in training pipelines.
    """
    shape = _frame_shape(source)
    frame_bytes = source.width * source.height * _PIX_FMT_CHANNELS[source.pix_fmt]
    # Capture stderr so a non-zero exit can surface ffmpeg's diagnostic
    # rather than the generic "ffmpeg failed" the caller would otherwise
    # have to debug from logs.
    proc: _PopenLike = popen(
        [
            ffmpeg,
            "-v",
            "error",
            "-i",
            str(source.path),
            "-f",
            "rawvideo",
            "-pix_fmt",
            source.pix_fmt,
            "-",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert proc.stdout is not None
    try:
        while True:
            buf = proc.stdout.read(frame_bytes)
            if len(buf) < frame_bytes:
                return
            yield np.frombuffer(buf, dtype=np.uint8).reshape(shape).copy()
    finally:
        try:
            proc.stdout.close()
        except OSError as exc:  # pragma: no cover — defensive
            _LOG.debug("stdout.close() during iter_frames cleanup raised %s", exc)
        try:
            rc = proc.wait(timeout=_FFMPEG_WAIT_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            # Don't leave a runaway ffmpeg dangling — kill it and recover.
            _LOG.warning(
                "ffmpeg failed to exit within %.1fs after stdout EOF for %s; killing",
                _FFMPEG_WAIT_TIMEOUT_S,
                source.path,
            )
            proc.kill()
            try:
                rc = proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                rc = -1
        stderr = b""
        if proc.stderr is not None:
            try:
                stderr = proc.stderr.read() or b""
            except OSError as exc:  # pragma: no cover — defensive
                _LOG.debug("stderr.read() during iter_frames cleanup raised %s", exc)
            finally:
                try:
                    proc.stderr.close()
                except OSError as exc:  # pragma: no cover — defensive
                    _LOG.debug("stderr.close() during iter_frames cleanup raised %s", exc)
        if rc != 0:
            diag = stderr.decode("utf-8", errors="replace").strip()[:500]
            raise RuntimeError(
                f"ffmpeg decode of {source.path} exited rc={rc}: {diag or '<no stderr>'}"
            )
