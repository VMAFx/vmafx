# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fixture corpus for the mini retrain (ADR-1898 / issue #1246).

The corpus is generated, never downloaded: it is cut from the 576x324
Big Buck Bunny pair the repository already tracks (``testdata/ref_576x324_48f.yuv``
and ``testdata/dis_576x324_48f.yuv``) and extended with two seeded synthetic
distortions of the reference. The layout is the one
``ai/data/netflix_loader.py`` reads, so the real extractor runs unchanged::

    <root>/ref/<Source>_<fps>fps.yuv
    <root>/dis/<Source>_<quality>_<height>_<kbps>.yuv

Four sources, three distorted variants each, ``CLIP_FRAMES`` frames per clip:
twelve pairs, 144 per-frame rows. Four sources are the minimum that leave
every leave-one-source-out fold with a training split of three.

Determinism: the same ``seed`` and the same input bytes give byte-identical
files (numpy ``default_rng`` PCG64, no wall-clock input).
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path

import numpy as np

WIDTH = 576
HEIGHT = 324
FPS = 24
CLIP_FRAMES = 12
SOURCES: tuple[str, ...] = ("SrcA", "SrcB", "SrcC", "SrcD")
FRAME_BYTES = WIDTH * HEIGHT * 3 // 2
_Y_BYTES = WIDTH * HEIGHT
_VARIANTS: tuple[tuple[str, int, int], ...] = (
    # (kind, quality label, bitrate label kbps) -> dis file name token
    ("codec", 30, 1000),
    ("noise", 20, 500),
    ("blur", 10, 250),
)


class MiniCorpusError(RuntimeError):
    """The fixture corpus cannot be built or fails its own check."""


@dataclass(frozen=True)
class MiniCorpus:
    """Where a generated corpus lives and what it holds."""

    root: Path
    pairs: int
    frames_per_clip: int
    sha256: str


def _read_frames(path: Path, first: int, count: int) -> np.ndarray:
    """Read ``count`` yuv420p frames starting at ``first`` as ``(count, FRAME_BYTES)``."""
    if not path.is_file():
        raise MiniCorpusError(f"input yuv missing: {path}")
    need = (first + count) * FRAME_BYTES
    if path.stat().st_size < need:
        raise MiniCorpusError(
            f"input yuv {path} holds {path.stat().st_size} bytes; frames "
            f"{first}..{first + count - 1} need {need}"
        )
    with path.open("rb") as fh:
        fh.seek(first * FRAME_BYTES)
        raw = fh.read(count * FRAME_BYTES)
    return np.frombuffer(raw, dtype=np.uint8).reshape(count, FRAME_BYTES).copy()


def _blur_luma(frames: np.ndarray) -> np.ndarray:
    """3x3 box blur of the luma plane of every frame; chroma is untouched."""
    out = frames.copy()
    for i in range(frames.shape[0]):
        y = frames[i, :_Y_BYTES].reshape(HEIGHT, WIDTH).astype(np.uint16)
        pad = np.pad(y, 1, mode="edge")
        acc = np.zeros_like(y)
        for dy in range(3):
            for dx in range(3):
                acc += pad[dy : dy + HEIGHT, dx : dx + WIDTH]
        out[i, :_Y_BYTES] = ((acc + 4) // 9).astype(np.uint8).reshape(-1)
    return out


def _noise_luma(frames: np.ndarray, rng: np.random.Generator, sigma: float) -> np.ndarray:
    """Add seeded Gaussian noise to the luma plane of every frame."""
    out = frames.copy()
    for i in range(frames.shape[0]):
        y = frames[i, :_Y_BYTES].astype(np.float64)
        y += rng.normal(0.0, sigma, size=y.shape)
        out[i, :_Y_BYTES] = np.clip(np.rint(y), 0, 255).astype(np.uint8)
    return out


def _variant(
    kind: str, ref: np.ndarray, real_dis: np.ndarray, rng: np.random.Generator
) -> np.ndarray:
    if kind == "codec":
        return real_dis
    if kind == "noise":
        return _noise_luma(ref, rng, sigma=9.0)
    if kind == "blur":
        return _blur_luma(ref)
    raise MiniCorpusError(f"unknown distortion kind {kind!r}")


def corpus_digest(root: Path) -> str:
    """SHA-256 over every file name and byte under ``root``, in sorted order."""
    h = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        h.update(str(path.relative_to(root)).encode())
        h.update(b"\0")
        h.update(path.read_bytes())
    return h.hexdigest()


def generate_mini_corpus(
    root: Path,
    *,
    ref_yuv: Path,
    dis_yuv: Path,
    seed: int = 0,
) -> MiniCorpus:
    """Write the fixture corpus under ``root`` and return its description."""
    ref_dir = root / "ref"
    dis_dir = root / "dis"
    ref_dir.mkdir(parents=True, exist_ok=True)
    dis_dir.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(seed)
    pairs = 0
    for index, source in enumerate(SOURCES):
        first = index * CLIP_FRAMES
        ref = _read_frames(ref_yuv, first, CLIP_FRAMES)
        real_dis = _read_frames(dis_yuv, first, CLIP_FRAMES)
        (ref_dir / f"{source}_{FPS}fps.yuv").write_bytes(ref.tobytes())
        for kind, quality, kbps in _VARIANTS:
            dis = _variant(kind, ref, real_dis, rng)
            name = f"{source}_{quality}_{HEIGHT}_{kbps}.yuv"
            (dis_dir / name).write_bytes(dis.tobytes())
            pairs += 1
    return MiniCorpus(
        root=root, pairs=pairs, frames_per_clip=CLIP_FRAMES, sha256=corpus_digest(root)
    )
