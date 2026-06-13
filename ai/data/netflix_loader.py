# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Loader for the Netflix VMAF training corpus.

The corpus lives at ``$VMAF_DATA_ROOT`` (or the ``--data-root`` CLI flag)
and follows the layout documented in `docs/ai/training-data.md`::

    <data-root>/
      ref/<source>_<fps>fps.yuv             # 9 reference YUVs
      dis/<source>_<quality>_<height>_<bitrate-kbps>.yuv

All shipped corpus YUVs are 1920x1080 ``yuv420p`` 8-bit regardless of
the ``<height>`` token in the filename — that token records the encode-
ladder rung *before* upscale to the reference resolution. The loader
verifies this by computing
``size == width * height * 1.5 * frames`` and falling back to ffprobe if
the size does not match the 1920x1080 assumption (see
:func:`probe_yuv_dims`).

Caching: per-clip feature / score JSONs land at
``~/.cache/vmaf-tiny-ai/<source>/<dis_basename>.json`` so repeated runs
skip the libvmaf pass. Override the cache root with
``VMAF_TINY_AI_CACHE``.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

# 1920x1080 YUV420p 8-bit -> 3110400 bytes/frame.
DEFAULT_W: int = 1920
DEFAULT_H: int = 1080
_FRAME_BYTES_1080P_420_8BIT = 1920 * 1080 * 3 // 2

# <source>_<quality>_<encode_height>_<bitrate>.yuv
_DIS_RE = re.compile(
    r"^(?P<source>[A-Za-z0-9]+)_(?P<quality>\d+)_(?P<height>\d+)_(?P<bitrate>\d+)\.yuv$"
)
# <source>_<fps>fps.yuv
_REF_RE = re.compile(r"^(?P<source>[A-Za-z0-9]+)_(?P<fps>\d+)fps\.yuv$")


@dataclass(frozen=True)
class NetflixPair:
    """One (reference, distorted) pair with metadata."""

    source: str  # e.g. "BigBuckBunny"
    ref_path: Path
    dis_path: Path
    width: int  # actual frame width (always 1920 in the shipped corpus)
    height: int  # actual frame height (always 1080)
    fps: int  # parsed from the reference filename
    encode_height: int  # ladder rung from the dis filename (288, 384, ..., 1080)
    quality: int  # opaque quality label from the dis filename
    bitrate_kbps: int  # bitrate label from the dis filename

    @property
    def cache_key(self) -> str:
        return f"{self.source}/{self.dis_path.stem}"


def _cache_root() -> Path:
    env = os.environ.get("VMAF_TINY_AI_CACHE")
    if env:
        return Path(env)
    return Path.home() / ".cache" / "vmaf-tiny-ai"


def cache_path_for(pair: NetflixPair) -> Path:
    """Return ``<cache_root>/<source>/<dis-stem>.json`` for caching."""
    root = _cache_root()
    return root / pair.source / f"{pair.dis_path.stem}.json"


def _ffprobe_dims(path: Path) -> tuple[int, int] | None:
    """Best-effort ffprobe call for unusual YUVs. ``None`` on any failure."""
    try:
        out = subprocess.run(
            [
                "ffprobe",
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream=width,height",
                "-of",
                "csv=p=0:s=x",
                str(path),
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=15,
        )
    except (subprocess.SubprocessError, FileNotFoundError):
        return None
    parts = out.stdout.strip().split("x")
    if len(parts) != 2:
        return None
    try:
        return int(parts[0]), int(parts[1])
    except ValueError:
        return None


def probe_yuv_dims(path: Path) -> tuple[int, int]:
    """Determine ``(width, height)`` for a YUV file.

    Strategy:

    1. If file size is an exact multiple of ``1920*1080*1.5``, assume the
       Netflix corpus default.
    2. Otherwise, try ffprobe.
    3. As a last resort, raise — never guess.
    """
    size = path.stat().st_size
    if size > 0 and size % _FRAME_BYTES_1080P_420_8BIT == 0:
        return DEFAULT_W, DEFAULT_H
    dims = _ffprobe_dims(path)
    if dims is not None:
        return dims
    raise RuntimeError(
        f"Cannot determine YUV dimensions for {path}; "
        "file size does not match 1920x1080 yuv420p and ffprobe is unavailable."
    )


def _parse_dis_filename(name: str) -> dict[str, int | str] | None:
    m = _DIS_RE.match(name)
    if m is None:
        return None
    return {
        "source": m.group("source"),
        "quality": int(m.group("quality")),
        "encode_height": int(m.group("height")),
        "bitrate_kbps": int(m.group("bitrate")),
    }


def _parse_ref_filename(name: str) -> dict[str, int | str] | None:
    m = _REF_RE.match(name)
    if m is None:
        return None
    return {"source": m.group("source"), "fps": int(m.group("fps"))}


def _load_refs(ref_dir: Path) -> dict[str, tuple[Path, int]]:
    """Index references by source name."""
    refs: dict[str, tuple[Path, int]] = {}
    if not ref_dir.is_dir():
        return refs
    for entry in sorted(ref_dir.iterdir()):
        if not entry.is_file() or entry.suffix != ".yuv":
            continue
        meta = _parse_ref_filename(entry.name)
        if meta is None:
            continue
        source = str(meta["source"])
        fps = int(meta["fps"])
        if source in refs:
            # Deterministic: keep the first one (sorted order) and warn via metadata.
            continue
        refs[source] = (entry, fps)
    return refs


def iter_pairs(
    data_root: Path,
    *,
    sources: tuple[str, ...] | None = None,
    max_pairs: int | None = None,
    assume_dims: tuple[int, int] | None = None,
) -> Iterator[NetflixPair]:
    """Yield :class:`NetflixPair` for every dis YUV with a matching ref.

    Args:
        data_root: directory containing ``ref/`` and ``dis/`` subfolders.
        sources: if non-None, restrict to these source names. Use this for
            unit-testable smoke runs (e.g. ``("BigBuckBunny",)``).
        max_pairs: cap the number of pairs yielded. Useful for the
            ``--epochs 0`` smoke command in CI.
        assume_dims: if non-None, skip the ``probe_yuv_dims`` check and
            stamp every pair with the given ``(width, height)``. Tests
            inject this to use synthetic 16x16 fixtures; production code
            should leave it as ``None`` so size mismatches are caught.
    """
    data_root = Path(data_root)
    ref_dir = data_root / "ref"
    dis_dir = data_root / "dis"
    if not dis_dir.is_dir():
        raise FileNotFoundError(f"Distorted directory not found: {dis_dir}")
    refs = _load_refs(ref_dir)
    yielded = 0
    for entry in sorted(dis_dir.iterdir()):
        if not entry.is_file() or entry.suffix != ".yuv":
            continue
        meta = _parse_dis_filename(entry.name)
        if meta is None:
            continue
        source = str(meta["source"])
        if sources is not None and source not in sources:
            continue
        if source not in refs:
            # Orphan: a dis without its ref. Skip silently — the caller
            # should not have to filter again.
            continue
        ref_path, fps = refs[source]
        if assume_dims is not None:
            width, height = assume_dims
        else:
            width, height = probe_yuv_dims(entry)
        yield NetflixPair(
            source=source,
            ref_path=ref_path,
            dis_path=entry,
            width=width,
            height=height,
            fps=fps,
            encode_height=int(meta["encode_height"]),
            quality=int(meta["quality"]),
            bitrate_kbps=int(meta["bitrate_kbps"]),
        )
        yielded += 1
        if max_pairs is not None and yielded >= max_pairs:
            return


def list_sources(data_root: Path) -> list[str]:
    """Return the sorted list of source names found under ``<root>/ref/``."""
    return sorted(_load_refs(Path(data_root) / "ref").keys())


def load_or_compute(
    pair: NetflixPair,
    compute_fn,  # type: ignore[no-untyped-def]
    *,
    use_cache: bool = True,
) -> dict:
    """Read the per-clip JSON cache or call ``compute_fn(pair) -> dict``.

    The compute function is only invoked on cache miss. The result is
    written to ``cache_path_for(pair)`` atomically (write-then-rename).
    """
    cache_file = cache_path_for(pair)
    if use_cache and cache_file.is_file():
        try:
            return json.loads(cache_file.read_text())
        except json.JSONDecodeError:
            # Corrupt cache — fall through to recompute and overwrite.
            pass
    payload = compute_fn(pair)
    if use_cache:
        cache_file.parent.mkdir(parents=True, exist_ok=True)
        tmp = cache_file.with_suffix(cache_file.suffix + ".tmp")
        tmp.write_text(json.dumps(payload))
        tmp.replace(cache_file)
    return payload
