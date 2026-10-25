# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Content-addressed cache for vmaf-tune trial results.

Iterative tuning workflows re-run the same `(src, encoder, preset, crf)`
tuples constantly — adjusting one flag and re-launching the corpus
sweep is the dominant interaction. Re-encoding and re-scoring the
unchanged tuples burns minutes-to-hours of wall clock for no new
information. This module turns those cells into a free hit.

Cache key components (every one required to invalidate correctly):

1. ``src_sha256``          — content hash of the reference YUV
2. ``encoder``             — adapter name (``libx264``, …)
3. ``preset``              — encoder preset string
4. ``crf``                 — quality knob value (int)
5. ``adapter_version``     — bumps when the adapter's argv shape changes
6. ``ffmpeg_version``      — host ffmpeg version string
7. ``passes``              — 1 or 2 (a 2-pass encode is another result)
8. ``sample_clip_seconds`` / ``sample_clip_start_s`` — the encoded window
9. ``settings``            — every other input of the encode and the score
   (geometry, duration, extra encoder argv, VMAF model, score backend)

The key is the SHA-256 of the canonical-JSON encoding of those fields
plus :data:`CACHE_VERSION`. Dropping any one of them produces wrong
cached scores when, e.g., the adapter is upgraded, ffmpeg is rebuilt or
a 1-pass result answers a 2-pass request — that's a bug, not an
optimisation, so the key signature is enforced by tests. An empty
adapter or ffmpeg version is refused: it would make every upgrade a hit.

Cache layout on disk::

    <cache-dir>/
      meta/<key>.json     — small JSON sidecar with the parsed result tuple
                            and the corpus row the miss produced
      blobs/<key>.bin     — opaque encoded artifact (atomic put: tmp + rename)

A single ``__index__.json`` carries last-access timestamps for LRU
eviction; it's rewritten whole on `put` / `evict_lru` (cheap because
entries are small and writes are infrequent compared to encodes).

Per ADR-0298, **the cache content is not baked into the JSONL row** —
the row stays the canonical record. The cache is an opaque
encode/score result store keyed by inputs.
"""

from __future__ import annotations

import contextlib
import dataclasses
import hashlib
import json
import math
import os
import shutil
import tempfile
import time
from collections.abc import Iterable, Mapping
from pathlib import Path
from typing import Any

from .jsonio import write_json_strict

# Bumps any time the cache key composition or the on-disk layout
# changes in a way that should invalidate older entries. 2: the key
# carries the adapter and ffmpeg versions, the pass count, the
# sample-clip window and the encode/score settings; entries store the
# miss row. Version-1 keys hash differently, so old entries miss.
CACHE_VERSION = 2

# Default ceiling — generous for typical workflows (~50-100 cells of
# 720p/1080p HEVC sit comfortably under 10 GB), small enough to not
# silently consume a workstation's home dir.
DEFAULT_SIZE_BYTES = 10 * 1024 * 1024 * 1024  # 10 GiB


@dataclasses.dataclass(frozen=True)
class NearDupRecord:
    """Provenance record when a result is reused from a near-duplicate match (ADR-3372)."""

    matched_src_sha256: str
    segment_ratios: tuple[float, float, float]
    matched_key: str = ""


@dataclasses.dataclass(frozen=True)
class CachedResult:
    """The parsed tuple a hit returns alongside the artifact path.

    Mirrors the subset of ``EncodeResult`` + ``ScoreResult`` fields the
    corpus row needs. We store the parsed values rather than the raw
    stderr blobs — those can balloon and are not load-bearing.
    """

    encode_size_bytes: int
    encode_time_ms: float
    encoder_version: str
    ffmpeg_version: str
    vmaf_score: float
    vmaf_model: str
    score_time_ms: float
    vmaf_binary_version: str
    artifact_path: Path
    # The corpus row the miss produced, without its provenance columns
    # (``run_id``, ``timestamp``, ``encode_path``); a hit replays it so
    # the hit row equals the miss row. ``None`` when the entry has none.
    row: Mapping[str, Any] | None = None
    # ADR-3372 near-duplicate match record when reused via perceptual fingerprint
    near_dup: NearDupRecord | None = None


def _xdg_cache_home() -> Path:
    """Resolve XDG cache root with the documented fallbacks."""
    env = os.environ.get("XDG_CACHE_HOME")
    if env:
        return Path(env).expanduser()
    return Path.home() / ".cache"


def default_cache_dir() -> Path:
    """``$XDG_CACHE_HOME/vmaf-tune`` (or ``~/.cache/vmaf-tune``)."""
    return _xdg_cache_home() / "vmaf-tune"


def _trial_digest(label: str, identity: Mapping[str, Any], params: Mapping[str, Any]) -> str:
    """Validate trial parameters and return the SHA-256 of their canonical JSON.

    ``identity`` holds the text fields that must be non-empty (the source
    hash for :func:`cache_key`, nothing more for :func:`params_key`).
    """
    texts = {
        **identity,
        "encoder": params["encoder"],
        "preset": params["preset"],
        "adapter_version": params["adapter_version"],
        "ffmpeg_version": params["ffmpeg_version"],
    }
    for name, value in texts.items():
        if not value:
            raise ValueError(f"{label} requires non-empty {name}")
    passes = params["passes"]
    if passes not in (1, 2):
        raise ValueError(f"{label}: passes must be 1 or 2, got {passes}")
    payload = {
        "v": CACHE_VERSION,
        **identity,
        "encoder": params["encoder"],
        "preset": params["preset"],
        "crf": int(params["crf"]),
        "adapter_version": params["adapter_version"],
        "ffmpeg_version": params["ffmpeg_version"],
        "passes": int(passes),
        "sample_clip_seconds": float(params["sample_clip_seconds"]),
        "sample_clip_start_s": float(params["sample_clip_start_s"]),
        "settings": dict(params["settings"] or {}),
    }
    blob = json.dumps(payload, sort_keys=True, separators=(",", ":"), allow_nan=False)
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()


def cache_key(
    *,
    src_sha256: str,
    encoder: str,
    preset: str,
    crf: int,
    adapter_version: str,
    ffmpeg_version: str,
    passes: int = 1,
    sample_clip_seconds: float = 0.0,
    sample_clip_start_s: float = 0.0,
    settings: Mapping[str, Any] | None = None,
) -> str:
    """Return the SHA-256 hex digest that identifies a trial.

    Every field is part of the digest — see module docstring. The text
    fields must be non-empty: an empty ``adapter_version`` or
    ``ffmpeg_version`` would let an adapter or ffmpeg upgrade reuse old
    results. ``settings`` holds the remaining encode and score inputs
    as JSON-serialisable values. Inputs are serialised through
    canonical JSON (``sort_keys=True``, ``separators=(",", ":")``) so
    the digest is stable across Python versions and dict orderings.
    """
    params = {
        "encoder": encoder,
        "preset": preset,
        "crf": crf,
        "adapter_version": adapter_version,
        "ffmpeg_version": ffmpeg_version,
        "passes": passes,
        "sample_clip_seconds": sample_clip_seconds,
        "sample_clip_start_s": sample_clip_start_s,
        "settings": settings,
    }
    return _trial_digest("cache_key", {"src_sha256": src_sha256}, params)


def params_key(
    *,
    encoder: str,
    preset: str,
    crf: int,
    adapter_version: str,
    ffmpeg_version: str,
    passes: int = 1,
    sample_clip_seconds: float = 0.0,
    sample_clip_start_s: float = 0.0,
    settings: Mapping[str, Any] | None = None,
) -> str:
    """Return the digest of :func:`cache_key`'s inputs without the source hash (ADR-3372).

    The secondary index key for near-duplicate fingerprint reuse.
    """
    params = {
        "encoder": encoder,
        "preset": preset,
        "crf": crf,
        "adapter_version": adapter_version,
        "ffmpeg_version": ffmpeg_version,
        "passes": passes,
        "sample_clip_seconds": sample_clip_seconds,
        "sample_clip_start_s": sample_clip_start_s,
        "settings": settings,
    }
    return _trial_digest("params_key", {}, params)


def _row_to_meta(row: Mapping[str, Any]) -> list[list[Any]]:
    """Encode a corpus row for the strict-JSON meta file.

    A list of ``[column, value]`` pairs keeps the row's column order
    (the meta file is written with sorted keys); non-finite floats
    become ``null``.
    """
    return [
        [key, None if isinstance(value, float) and not math.isfinite(value) else value]
        for key, value in row.items()
    ]


def _row_from_meta(raw: object) -> dict[str, Any] | None:
    """Decode a stored row; ``null`` (a corpus row's NaN) becomes NaN again.

    Returns ``None`` for anything that is not a list of ``[column, value]``
    pairs, which the corpus runner treats as a miss.
    """
    if not isinstance(raw, list):
        return None
    row: dict[str, Any] = {}
    for pair in raw:
        if not (isinstance(pair, list) and len(pair) == 2 and isinstance(pair[0], str)):
            return None
        row[pair[0]] = math.nan if pair[1] is None else pair[1]
    return row


class TuneCache:
    """Content-addressed store for vmaf-tune trial results.

    Thread-safety: not thread-safe. The corpus loop is single-threaded
    by design; concurrent runs against the same cache dir from
    separate processes work for reads but races on `put` may overwrite
    each other (last writer wins, both rows are valid by content
    addressing).
    """

    INDEX_NAME = "__index__.json"
    META_DIR = "meta"
    BLOB_DIR = "blobs"
    FINGERPRINT_DIR = "fingerprints"

    def __init__(
        self,
        path: Path | None = None,
        *,
        size_bytes: int = DEFAULT_SIZE_BYTES,
    ) -> None:
        self.path = Path(path) if path is not None else default_cache_dir()
        self.size_bytes = int(size_bytes)
        self.path.mkdir(parents=True, exist_ok=True)
        (self.path / self.META_DIR).mkdir(exist_ok=True)
        (self.path / self.BLOB_DIR).mkdir(exist_ok=True)
        (self.path / self.FINGERPRINT_DIR).mkdir(exist_ok=True)
        self._index_cache: dict[str, float] | None = None
        self._index_dirty = False

    # -- index helpers -----------------------------------------------

    def _index_path(self) -> Path:
        return self.path / self.INDEX_NAME

    def _read_index(self) -> dict[str, float]:
        """Load index into memory cache on first access.

        Subsequent accesses return the in-memory copy; writes are
        deferred via the dirty flag until ``flush()`` is called.
        """
        if self._index_cache is not None:
            return self._index_cache

        idx = self._index_path()
        if not idx.exists():
            self._index_cache = {}
            return {}
        try:
            with idx.open("r", encoding="utf-8") as fh:
                data = json.load(fh)
            if isinstance(data, dict):
                # values are floats (epoch seconds); coerce defensively
                self._index_cache = {str(k): float(v) for k, v in data.items()}
                return self._index_cache
        except (OSError, TypeError, ValueError, json.JSONDecodeError):
            # Corrupt index — rebuild from filesystem on next flush.
            pass
        self._index_cache = {}
        return {}

    def _write_index(self, index: dict[str, float]) -> None:
        """Flush index to disk and clear the dirty flag."""
        write_json_strict(self._index_path(), index, indent=None, trailing_newline=False)
        self._index_dirty = False

    def _meta_path(self, key: str) -> Path:
        return self.path / self.META_DIR / f"{key}.json"

    def _blob_path(self, key: str) -> Path:
        return self.path / self.BLOB_DIR / f"{key}.bin"

    def _fp_path(self, key: str) -> Path:
        return self.path / self.FINGERPRINT_DIR / f"{key}.json"

    # -- public API --------------------------------------------------

    def get(self, key: str) -> CachedResult | None:
        """Return the cached result for ``key``, or ``None`` on miss."""
        meta = self._meta_path(key)
        blob = self._blob_path(key)
        if not (meta.exists() and blob.exists()):
            return None
        try:
            with meta.open("r", encoding="utf-8") as fh:
                payload = json.load(fh)
        except (OSError, json.JSONDecodeError):
            return None
        if not isinstance(payload, dict):
            return None

        try:
            result = CachedResult(
                encode_size_bytes=int(payload.get("encode_size_bytes", 0)),
                encode_time_ms=float(payload.get("encode_time_ms", 0.0)),
                encoder_version=str(payload.get("encoder_version", "")),
                ffmpeg_version=str(payload.get("ffmpeg_version", "")),
                vmaf_score=float(payload["vmaf_score"]),
                vmaf_model=str(payload.get("vmaf_model", "")),
                score_time_ms=float(payload.get("score_time_ms", 0.0)),
                vmaf_binary_version=str(payload.get("vmaf_binary_version", "")),
                artifact_path=blob,
                row=_row_from_meta(payload.get("row")),
            )
        except (KeyError, TypeError, ValueError):
            return None

        # Refresh LRU access time in memory, mark dirty for deferred flush.
        index = self._read_index()
        index[key] = time.time()
        self._index_dirty = True

        return result

    def put(
        self,
        key: str,
        result: CachedResult,
        artifact_path: Path,
        *,
        fingerprint: Any | None = None,
        params_key: str | None = None,
        src_sha256: str | None = None,
    ) -> CachedResult:
        """Store ``artifact_path`` + parsed ``result`` under ``key``.

        The artifact is copied (not moved) so callers retain ownership
        of the original. Returns a refreshed ``CachedResult`` whose
        ``artifact_path`` points at the cached copy. When ``fingerprint``
        is provided, a sidecar JSON is stored alongside for near-dup reuse.
        """
        if not artifact_path.exists():
            raise FileNotFoundError(f"artifact missing: {artifact_path}")

        meta = self._meta_path(key)
        blob = self._blob_path(key)

        # Atomic blob put: copy to tmp in the same dir, then rename.
        tmp_blob = blob.with_suffix(".bin.tmp")
        shutil.copyfile(artifact_path, tmp_blob)
        tmp_blob.replace(blob)

        payload = {
            "encode_size_bytes": int(result.encode_size_bytes),
            "encode_time_ms": float(result.encode_time_ms),
            "encoder_version": result.encoder_version,
            "ffmpeg_version": result.ffmpeg_version,
            "vmaf_score": float(result.vmaf_score),
            "vmaf_model": result.vmaf_model,
            "score_time_ms": float(result.score_time_ms),
            "vmaf_binary_version": result.vmaf_binary_version,
        }
        if result.row is not None:
            payload["row"] = _row_to_meta(result.row)
        write_json_strict(meta, payload, indent=None, trailing_newline=False)

        if fingerprint is not None:
            fp_raw = fingerprint.to_dict() if hasattr(fingerprint, "to_dict") else fingerprint
            fp_payload = {
                "src_sha256": str(src_sha256 or ""),
                "params_key": str(params_key or ""),
                "fingerprint": fp_raw,
            }
            write_json_strict(self._fp_path(key), fp_payload, indent=None, trailing_newline=False)

        index = self._read_index()
        index[key] = time.time()
        self._index_dirty = True

        if self.size_bytes > 0:
            self.evict_lru(self.size_bytes)

        return dataclasses.replace(result, artifact_path=blob)

    def flush(self) -> None:
        """Write the index to disk if it has been modified.

        Call this at the end of a batch operation (e.g., at the end of
        a corpus sweep) to persist pending changes. If the index is not
        dirty, this is a no-op.
        """
        if not self._index_dirty or self._index_cache is None:
            return
        with contextlib.suppress(OSError):
            self._write_index(self._index_cache)

    def total_bytes(self) -> int:
        """Sum of meta + blob bytes across all entries."""
        total = 0
        for sub in (self.META_DIR, self.BLOB_DIR, self.FINGERPRINT_DIR):
            d = self.path / sub
            if not d.exists():
                continue
            for f in d.iterdir():
                if f.is_file():
                    with contextlib.suppress(OSError):
                        total += f.stat().st_size
        return total

    def keys(self) -> Iterable[str]:
        meta_dir = self.path / self.META_DIR
        if not meta_dir.exists():
            return []
        return sorted(p.stem for p in meta_dir.iterdir() if p.suffix == ".json")

    def evict_lru(self, target_bytes: int) -> int:
        """Drop oldest entries until total size ≤ ``target_bytes``.

        Returns the number of entries evicted. Entries with no index
        timestamp are treated as oldest (tie-broken by key).
        """
        if target_bytes <= 0:
            # Caller asked for unbounded → no-op.
            return 0
        index = self._read_index()

        evicted = 0
        # Build sorted (timestamp, key) — oldest first.
        keys_sorted = sorted(self.keys(), key=lambda k: (index.get(k, 0.0), k))

        while self.total_bytes() > target_bytes and keys_sorted:
            victim = keys_sorted.pop(0)
            self._drop(victim)
            index.pop(victim, None)
            evicted += 1

        if evicted:
            with contextlib.suppress(OSError):
                self._write_index(index)
        return evicted

    def _drop(self, key: str) -> None:
        for p in (self._meta_path(key), self._blob_path(key), self._fp_path(key)):
            with contextlib.suppress(OSError):
                p.unlink()

    def get_near_duplicate(
        self,
        fingerprint: Any,
        params_key_str: str,
    ) -> tuple[CachedResult, NearDupRecord] | None:
        """Find cached result for near-duplicate source matching fingerprint and params_key."""
        from .fingerprint import VideoFingerprint, match

        query_fp = (
            fingerprint
            if isinstance(fingerprint, VideoFingerprint)
            else VideoFingerprint.from_dict(fingerprint)
        )
        fp_dir = self.path / self.FINGERPRINT_DIR
        if not fp_dir.exists():
            return None

        index = self._read_index()
        keys_sorted = sorted(self.keys(), key=lambda k: index.get(k, 0.0), reverse=True)

        for candidate_key in keys_sorted:
            fp_path = self._fp_path(candidate_key)
            if not fp_path.exists():
                continue
            try:
                with fp_path.open("r", encoding="utf-8") as fh:
                    entry = json.load(fh)
            except (OSError, json.JSONDecodeError):
                continue
            if not isinstance(entry, dict) or entry.get("params_key") != params_key_str:
                continue

            raw_fp = entry.get("fingerprint")
            if not isinstance(raw_fp, dict):
                continue
            try:
                candidate_fp = VideoFingerprint.from_dict(raw_fp)
            except (KeyError, TypeError, ValueError):
                continue

            match_res = match(query_fp, candidate_fp)
            if match_res.accepted:
                cached = self.get(candidate_key)
                if cached is not None:
                    record = NearDupRecord(
                        matched_src_sha256=str(entry.get("src_sha256", "")),
                        segment_ratios=match_res.segment_ratios,
                        matched_key=candidate_key,
                    )
                    cached_with_record = dataclasses.replace(cached, near_dup=record)
                    return cached_with_record, record
        return None

    # -- ergonomic helpers used by `corpus.py` -----------------------

    @staticmethod
    def make_artifact_blob(payload: bytes, dst: Path) -> Path:
        """Helper used by tests to fabricate an opaque artifact byte
        blob in a tmp dir so ``put`` has something to copy in.
        """
        dst.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile("wb", dir=dst.parent, delete=False) as fh:
            fh.write(payload)
            tmp = Path(fh.name)
        tmp.replace(dst)
        return dst


__all__ = [
    "CACHE_VERSION",
    "DEFAULT_SIZE_BYTES",
    "CachedResult",
    "NearDupRecord",
    "TuneCache",
    "cache_key",
    "default_cache_dir",
    "params_key",
]
