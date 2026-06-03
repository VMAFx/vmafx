#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared infrastructure for MOS-corpus JSONL ingestion adapters (ADR-0371).

All six MOS-corpus adapters (KonViD-1k, KonViD-150k, LSVQ, LIVE-VQC,
Waterloo IVC 4K-VQA, YouTube UGC) duplicated ~200 lines of identical
boilerplate.  This module consolidates those into a single place:

* :func:`sha256_file`           — chunked SHA-256 with 1 MiB reads
* :func:`utc_now_iso`           — second-precision ISO-8601 UTC timestamp
* :func:`probe_geometry`        — ffprobe JSON geometry extractor
* :func:`pick`                  — case-insensitive CSV column picker
* :func:`normalise_clip_name`   — append default suffix to bare stems
* :func:`load_progress`         — resumable-download state reader
* :func:`save_progress`         — atomic progress JSON writer
* :func:`mark_done`             — mark a clip as successfully downloaded
* :func:`mark_failed`           — mark a clip as non-retriably failed
* :func:`should_attempt`        — decide whether to (re-)attempt a clip
* :func:`download_clip`         — curl-backed per-clip downloader
* :class:`RunStats`             — aggregate run counters
* :class:`CorpusIngestBase`     — orchestrator base class (ABC)

Each adapter subclass overrides :meth:`CorpusIngestBase.iter_source_rows`
to produce ``(clip_path, manifest_row)`` pairs for its corpus-specific
CSV/manifest shape, then calls :meth:`CorpusIngestBase.run` which handles
the shared probe-SHA-write-dedup loop.
"""

from __future__ import annotations

import contextlib
import datetime as _dt
import hashlib
import json
import logging
import os
import subprocess
import tempfile
from abc import ABC, abstractmethod
from collections.abc import Callable, Iterator, Mapping, Sequence
from pathlib import Path
from typing import Any

from aiutils.run_manifest import build_run_provenance, normalise_manifest_value, write_manifest_json

_LOG = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Module-level constants
# ---------------------------------------------------------------------------

#: SHA-256 read chunk (1 MiB) — matches the existing ``manifest_scan``
#: reader in :mod:`ai.src.vmaf_train.data.manifest_scan`.
_SHA_CHUNK_BYTES: int = 1 << 20

#: Download-state literals used across all adapters.
STATE_DONE: str = "done"
STATE_FAILED: str = "failed"


# ---------------------------------------------------------------------------
# Small pure helpers
# ---------------------------------------------------------------------------


def utc_now_iso() -> str:
    """Return the current time as an ISO-8601 UTC string, second-precision."""
    return _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat()


def sha256_file(path: Path) -> str:
    """Stream a chunked SHA-256 over ``path`` and return the hex digest."""
    h = hashlib.sha256()
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(_SHA_CHUNK_BYTES)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def pick(row: dict[str, str], keys: tuple[str, ...] | list[str]) -> str | None:
    """Return the first non-empty value at any of ``keys`` (case-insensitive).

    Shared by every CSV parser in the MOS-corpus adapter family.
    """
    lower = {k.lower(): k for k in row}
    for key in keys:
        actual = lower.get(key.lower())
        if actual is None:
            continue
        val = row[actual]
        if val is None:
            continue
        s = str(val).strip()
        if s:
            return s
    return None


def normalise_clip_name(stem: str, *, suffix: str = ".mp4") -> str:
    """Return ``stem`` with ``suffix`` appended if it has no extension.

    Many manifests store bare stems (``"0001"``) rather than full
    filenames (``"0001.mp4"``). This helper makes both shapes accepted.
    """
    if "." in stem:
        return stem
    return stem + suffix


def _parse_framerate(rate: str) -> float:
    """Parse an ffprobe rational ``a/b`` or plain float framerate string."""
    if not rate:
        return 0.0
    if "/" in rate:
        num_s, den_s = rate.split("/", 1)
        try:
            num = float(num_s)
            den = float(den_s)
        except ValueError:
            return 0.0
        if den == 0.0:
            return 0.0
        return num / den
    try:
        return float(rate)
    except ValueError:
        return 0.0


# ---------------------------------------------------------------------------
# ffprobe geometry probe
# ---------------------------------------------------------------------------


#: Wall-clock cap (seconds) for a single ffprobe geometry probe. A wedged
#: ffprobe (corrupt input demuxer loop, NFS hang) would otherwise stall the
#: whole ingest run indefinitely. 60 s is generous: a healthy probe is
#: sub-second; legitimate slow probes (large 4K MOV with cold cache) finish
#: under 5 s.
_PROBE_GEOMETRY_TIMEOUT_S: float = 60.0


def probe_geometry(
    clip_path: Path,
    *,
    ffprobe_bin: str = "ffprobe",
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    timeout_s: float = _PROBE_GEOMETRY_TIMEOUT_S,
) -> dict[str, Any] | None:
    """Return a geometry dict for the first video stream in ``clip_path``.

    The dict has keys ``width``, ``height``, ``framerate``,
    ``duration_s``, ``pix_fmt``, and ``encoder_upstream``.  Returns
    ``None`` on any failure (bad rc, no stream, JSON parse error,
    subprocess timeout).

    The ``runner`` kwarg is a test seam; production callers leave it as
    the default :func:`subprocess.run`.  ``timeout_s`` caps the wall-clock
    for a single ffprobe invocation; on timeout the function logs a
    warning and returns ``None`` (the clip is treated as broken).
    """
    cmd = [
        ffprobe_bin,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height,r_frame_rate,avg_frame_rate,duration,pix_fmt,codec_name",
        "-show_entries",
        "format=duration",
        "-of",
        "json",
        str(clip_path),
    ]
    try:
        proc = runner(cmd, check=False, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        _LOG.warning("ffprobe timed out after %.1fs for %s", timeout_s, clip_path.name)
        return None
    except (FileNotFoundError, OSError) as exc:
        _LOG.warning("ffprobe spawn failed for %s: %s", clip_path.name, exc)
        return None

    rc = getattr(proc, "returncode", 1)
    stdout = getattr(proc, "stdout", "") or ""
    if rc != 0:
        _LOG.warning(
            "ffprobe rc=%d for %s; stderr=%s",
            rc,
            clip_path.name,
            (getattr(proc, "stderr", "") or "").strip()[:200],
        )
        return None

    try:
        payload = json.loads(stdout)
    except json.JSONDecodeError as exc:
        _LOG.warning("ffprobe non-JSON output for %s: %s", clip_path.name, exc)
        return None

    streams = payload.get("streams") or []
    if not streams:
        _LOG.warning("ffprobe: no video streams in %s", clip_path.name)
        return None
    s = streams[0]

    width = int(s.get("width", 0) or 0)
    height = int(s.get("height", 0) or 0)
    fps_raw = s.get("avg_frame_rate") or s.get("r_frame_rate") or ""
    framerate = _parse_framerate(fps_raw)

    duration_s = 0.0
    for src in (s, payload.get("format") or {}):
        d = src.get("duration")
        if d is None:
            continue
        try:
            duration_s = float(d)
        except (TypeError, ValueError):
            continue
        if duration_s > 0:
            break

    return {
        "width": width,
        "height": height,
        "framerate": framerate,
        "duration_s": duration_s,
        "pix_fmt": str(s.get("pix_fmt") or ""),
        "encoder_upstream": str(s.get("codec_name") or ""),
    }


# ---------------------------------------------------------------------------
# Resumable-download progress state
# ---------------------------------------------------------------------------


def load_progress(progress_path: Path) -> dict[str, dict[str, Any]]:
    """Load the download-progress JSON from ``progress_path``.

    Returns ``{filename: {"state": "done"|"failed", ...}}``.
    Missing / unreadable files return an empty dict so re-runs treat
    every URL as ``pending``.
    """
    if not progress_path.is_file():
        return {}
    try:
        raw = json.loads(progress_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        _LOG.warning("progress file %s unreadable (%s); starting fresh", progress_path, exc)
        return {}
    if not isinstance(raw, dict):
        _LOG.warning("progress file %s has wrong shape; starting fresh", progress_path)
        return {}
    out: dict[str, dict[str, Any]] = {}
    for key, val in raw.items():
        if isinstance(key, str) and isinstance(val, dict):
            out[key] = val
    return out


def save_progress(progress_path: Path, state: dict[str, dict[str, Any]]) -> None:
    """Atomically write the download-progress JSON via tempfile + rename.

    An interrupted write never leaves a half-truncated file the next run
    would discard.
    """
    progress_path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_str = tempfile.mkstemp(
        prefix=".download-progress.", suffix=".tmp", dir=str(progress_path.parent)
    )
    tmp_path = Path(tmp_str)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            json.dump(state, fh, sort_keys=True, indent=2)
            fh.write("\n")
        tmp_path.replace(progress_path)
    except Exception:
        with contextlib.suppress(OSError):
            tmp_path.unlink()
        raise


def mark_done(state: dict[str, dict[str, Any]], filename: str) -> None:
    """Mark ``filename`` as successfully downloaded in ``state``."""
    state[filename] = {"state": STATE_DONE}


def mark_failed(state: dict[str, dict[str, Any]], filename: str, reason: str) -> None:
    """Mark ``filename`` as a non-retriable download failure in ``state``."""
    state[filename] = {"state": STATE_FAILED, "reason": reason}


def should_attempt(state: dict[str, dict[str, Any]], filename: str, clip_path: Path) -> bool:
    """Return True if we should (re-)attempt downloading ``filename``.

    Decision table:

    * ``pending`` (no state entry): yes.
    * ``done`` and clip is on disk: no (already have it).
    * ``done`` but clip missing on disk: yes (re-fetch).
    * ``failed``: no — non-retriable; the operator must delete the progress
      file to retry.
    """
    entry = state.get(filename)
    if entry is None:
        return True
    s = entry.get("state")
    if s == STATE_FAILED:
        return False
    if s == STATE_DONE:
        return not clip_path.is_file()
    # Unknown state — conservative retry.
    return True


# ---------------------------------------------------------------------------
# curl-backed downloader
# ---------------------------------------------------------------------------


def download_clip(
    *,
    url: str,
    dest: Path,
    curl_bin: str = "curl",
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    timeout_s: int = 120,
) -> tuple[bool, str]:
    """Download ``url`` to ``dest`` via curl.

    Returns ``(ok, reason)`` where ``reason`` is a short diagnostic on
    failure (HTTP code, curl exit code) or empty on success.  Writes to a
    sibling ``.part`` file and renames atomically so a ``Ctrl-C``
    mid-download never leaves the dest appearing complete.

    The ``runner`` kwarg is the test seam.
    """
    if not url:
        return False, "no-url-in-manifest"
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_suffix(dest.suffix + ".part")
    cmd = [
        curl_bin,
        "--location",
        "--fail",
        "--silent",
        "--show-error",
        "--max-time",
        str(timeout_s),
        "--output",
        str(part),
        url,
    ]
    # Cap the runner wall-clock generously above curl's own ``--max-time``
    # so that ``curl --max-time`` is the authoritative timeout for a healthy
    # process, but a wedged DNS resolver / process-spawn / signal-handler
    # path cannot stall the ingest run forever.
    try:
        proc = runner(
            cmd, check=False, capture_output=True, text=True, timeout=float(timeout_s) + 30.0
        )
    except subprocess.TimeoutExpired:
        with contextlib.suppress(OSError):
            part.unlink()
        return False, f"curl-spawn-timeout: exceeded {timeout_s}s+30s"
    except (FileNotFoundError, OSError) as exc:
        return False, f"curl-spawn-failed: {exc}"

    rc = getattr(proc, "returncode", 1)
    if rc != 0:
        with contextlib.suppress(OSError):
            part.unlink()
        stderr = (getattr(proc, "stderr", "") or "").strip()
        return False, f"curl-rc={rc}: {stderr[:200]}"

    if not part.is_file() or part.stat().st_size == 0:
        with contextlib.suppress(OSError):
            part.unlink()
        return False, "curl-empty-output"

    try:
        part.replace(dest)
    except OSError as exc:
        return False, f"rename-failed: {exc}"
    return True, ""


# ---------------------------------------------------------------------------
# JSONL SHA index
# ---------------------------------------------------------------------------


def read_sha_index(jsonl_path: Path) -> set[str]:
    """Return the ``src_sha256`` values already present in ``jsonl_path``.

    Used for resume / dedup on re-runs (the file is append-only; this
    prevents re-emitting rows for files already ingested).  Tolerates
    malformed lines by skipping them.
    """
    if not jsonl_path.is_file():
        return set()
    seen: set[str] = set()
    with jsonl_path.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            sha = obj.get("src_sha256")
            if isinstance(sha, str) and sha:
                seen.add(sha)
    return seen


# ---------------------------------------------------------------------------
# RunStats
# ---------------------------------------------------------------------------


class RunStats:
    """Aggregate run counters returned by :meth:`CorpusIngestBase.run`.

    Attributes
    ----------
    written : int
        Rows successfully written to the JSONL.
    skipped_download : int
        Clips skipped because the download failed or was already marked
        ``failed`` in the progress file.
    skipped_broken : int
        Clips that were on disk but ffprobe rejected (zero geometry, bad
        codec, etc.).
    dedups : int
        Clips already present in the JSONL (keyed by ``src_sha256``);
        not re-emitted.
    attrition_pct : float
        ``skipped_download / total_rows``, set at the end of
        :meth:`CorpusIngestBase.run`.
    """

    __slots__ = ("attrition_pct", "dedups", "skipped_broken", "skipped_download", "written")

    def __init__(self) -> None:
        self.written = 0
        self.skipped_download = 0
        self.skipped_broken = 0
        self.dedups = 0
        self.attrition_pct = 0.0

    def as_tuple(self) -> tuple[int, int, int, int]:
        """Return ``(written, skipped_download, skipped_broken, dedups)``."""
        return (self.written, self.skipped_download, self.skipped_broken, self.dedups)

    def as_dict(self) -> dict[str, int | float]:
        """Return JSON-ready run counters."""
        return {
            "written": self.written,
            "skipped_download": self.skipped_download,
            "skipped_broken": self.skipped_broken,
            "dedups": self.dedups,
            "attrition_pct": self.attrition_pct,
        }


def _normalise_stats(stats: RunStats | Mapping[str, Any]) -> dict[str, Any]:
    if isinstance(stats, RunStats):
        return stats.as_dict()
    return dict(stats)


def write_ingest_manifest(
    path: Path,
    *,
    schema: str,
    entrypoint: Path,
    repo_root: Path,
    argv: Sequence[str],
    args: Any,
    corpus_label: str,
    stats: RunStats | Mapping[str, Any],
    inputs: Mapping[str, Any],
    outputs: Mapping[str, Any],
    config: Mapping[str, Any] | None = None,
) -> None:
    """Write a replay manifest for a MOS-corpus JSONL ingest run."""
    payload: dict[str, Any] = {
        "schema": schema,
        "corpus": corpus_label,
        "stats": normalise_manifest_value(_normalise_stats(stats)),
        "config": normalise_manifest_value(config or {}),
        "run_provenance": build_run_provenance(
            entrypoint=entrypoint,
            repo_root=repo_root,
            argv=argv,
            args=args,
            inputs=inputs,
            outputs=outputs,
        ),
    }
    write_manifest_json(path, payload)


# ---------------------------------------------------------------------------
# CorpusIngestBase ABC
# ---------------------------------------------------------------------------


class CorpusIngestBase(ABC):
    """Abstract base class for MOS-corpus JSONL ingestion adapters.

    Subclasses implement :meth:`iter_source_rows` to yield
    ``(clip_path, manifest_row_dict)`` pairs from their corpus-specific
    manifest format.  :meth:`run` orchestrates the probe-SHA-write-dedup
    loop and progress tracking.

    The ``manifest_row_dict`` yielded by :meth:`iter_source_rows` must
    contain at minimum:

    * ``"mos"`` — float MOS value (scale is corpus-defined)
    * ``"mos_std_dev"`` — float standard deviation (0.0 if absent)
    * ``"n_ratings"`` — int rating count (0 if absent)
    * ``"url"`` — str download URL (empty string if none)

    All other JSONL fields (``src``, ``src_sha256``, geometry, etc.) are
    populated by :meth:`run` via the shared helpers in this module.
    """

    #: JSONL ``corpus`` field literal — subclasses must override.
    corpus_label: str = ""

    def __init__(
        self,
        *,
        corpus_dir: Path,
        output: Path,
        manifest_csv: Path | None = None,
        progress_path: Path | None = None,
        clips_subdir: str = "clips",
        ffprobe_bin: str = "ffprobe",
        curl_bin: str = "curl",
        corpus_version: str = "",
        runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
        now_fn: Callable[[], str] = utc_now_iso,
        attrition_warn_threshold: float = 0.10,
        download_timeout_s: int = 120,
        max_rows: int | None = None,
        log: logging.Logger | None = None,
    ) -> None:
        self.corpus_dir = corpus_dir
        self.output = output
        self.manifest_csv = manifest_csv or (corpus_dir / "manifest.csv")
        self.progress_path = progress_path or (corpus_dir / ".download-progress.json")
        self.clips_subdir = clips_subdir
        self.ffprobe_bin = ffprobe_bin
        self.curl_bin = curl_bin
        self.corpus_version = corpus_version
        self.runner = runner
        self.now_fn = now_fn
        self.attrition_warn_threshold = attrition_warn_threshold
        self.download_timeout_s = download_timeout_s
        self.max_rows = max_rows
        self._log = log or _LOG

    # ------------------------------------------------------------------
    # Subclass contract
    # ------------------------------------------------------------------

    @abstractmethod
    def iter_source_rows(self, clips_dir: Path) -> Iterator[tuple[Path, dict[str, Any]]]:
        """Yield ``(clip_path, row_dict)`` for each entry in the manifest.

        ``clip_path`` is the resolved local path to the clip (it may not
        yet exist on disk — the orchestrator will attempt a download).

        ``row_dict`` must contain ``"mos"``, ``"mos_std_dev"``,
        ``"n_ratings"``, and ``"url"`` at minimum (other corpus-specific
        keys are ignored by the base orchestrator).
        """

    # ------------------------------------------------------------------
    # Shared helpers used by subclasses
    # ------------------------------------------------------------------

    def clips_dir_path(self) -> Path:
        """Return (and create) the ``clips/`` subdirectory."""
        p = self.corpus_dir / self.clips_subdir
        p.mkdir(parents=True, exist_ok=True)
        return p

    def _build_jsonl_row(
        self,
        clip_path: Path,
        manifest_row: dict[str, Any],
        geometry: dict[str, Any],
        ingested_at_utc: str,
        src_sha256: str,
    ) -> dict[str, Any]:
        """Assemble one output JSONL row from probed geometry + manifest data."""
        return {
            "src": clip_path.name,
            "src_sha256": src_sha256,
            "src_size_bytes": int(clip_path.stat().st_size),
            "width": int(geometry["width"]),
            "height": int(geometry["height"]),
            "framerate": float(geometry["framerate"]),
            "duration_s": float(geometry["duration_s"]),
            "pix_fmt": geometry["pix_fmt"],
            "encoder_upstream": geometry["encoder_upstream"],
            "mos": float(manifest_row["mos"]),
            "mos_std_dev": float(manifest_row.get("mos_std_dev", 0.0)),
            "n_ratings": int(manifest_row.get("n_ratings", 0)),
            "corpus": self.corpus_label,
            "corpus_version": self.corpus_version,
            "ingested_at_utc": ingested_at_utc,
        }

    # ------------------------------------------------------------------
    # Run orchestrator
    # ------------------------------------------------------------------

    def run(self) -> RunStats:
        """Execute the ingest loop and return aggregate :class:`RunStats`.

        Steps for each manifest row:

        1. If the clip is not on disk, attempt to download it via curl
           (respecting the resumable-download progress state).
        2. Probe geometry via ffprobe; skip if unusable.
        3. SHA-256 the clip; skip if already in the output JSONL.
        4. Append one JSON row to the output.
        5. Flush the progress state periodically and at the end.
        """
        if not self.corpus_dir.is_dir():
            raise FileNotFoundError(f"Corpus directory not found: {self.corpus_dir}")

        clips_dir = self.clips_dir_path()

        state = load_progress(self.progress_path)
        if state:
            already_done = sum(1 for v in state.values() if v.get("state") == STATE_DONE)
            already_failed = sum(1 for v in state.values() if v.get("state") == STATE_FAILED)
            self._log.info(
                "resume: %d clips done, %d clips failed (from %s)",
                already_done,
                already_failed,
                self.progress_path,
            )

        self.output.parent.mkdir(parents=True, exist_ok=True)
        seen_sha = read_sha_index(self.output)
        if seen_sha:
            self._log.info("resume: %d existing rows already in %s", len(seen_sha), self.output)

        ingested_at_utc = self.now_fn()
        stats = RunStats()
        saves_since_flush = 0
        rows_iter = self.iter_source_rows(clips_dir)
        rows: list[tuple[Path, dict[str, Any]]] = list(rows_iter)

        if self.max_rows is not None and len(rows) > self.max_rows:
            self._log.info(
                "capping manifest at max_rows=%d (full CSV had %d)",
                self.max_rows,
                len(rows),
            )
            rows = rows[: self.max_rows]

        total = len(rows)

        with self.output.open("a", encoding="utf-8") as fp:
            for idx, (clip_path, manifest_row) in enumerate(rows, start=1):
                filename = clip_path.name

                # Step 1: ensure the clip is on disk.
                if not clip_path.is_file():
                    if not should_attempt(state, filename, clip_path):
                        stats.skipped_download += 1
                        continue
                    url = manifest_row.get("url", "")
                    ok, reason = download_clip(
                        url=url,
                        dest=clip_path,
                        curl_bin=self.curl_bin,
                        runner=self.runner,
                        timeout_s=self.download_timeout_s,
                    )
                    if not ok:
                        self._log.warning("download failed for %s: %s", filename, reason)
                        mark_failed(state, filename, reason)
                        stats.skipped_download += 1
                        saves_since_flush += 1
                        if saves_since_flush >= 50:
                            save_progress(self.progress_path, state)
                            saves_since_flush = 0
                        continue
                    mark_done(state, filename)
                    saves_since_flush += 1
                else:
                    if state.get(filename, {}).get("state") != STATE_DONE:
                        mark_done(state, filename)
                        saves_since_flush += 1

                # Step 2: probe geometry.
                geometry = probe_geometry(
                    clip_path, ffprobe_bin=self.ffprobe_bin, runner=self.runner
                )
                if geometry is None:
                    stats.skipped_broken += 1
                    continue
                if geometry["width"] <= 0 or geometry["height"] <= 0:
                    self._log.warning("ffprobe returned zero geometry for %s; skipping", filename)
                    stats.skipped_broken += 1
                    continue

                # Step 3: SHA-256 and dedup.
                sha = sha256_file(clip_path)
                if sha in seen_sha:
                    stats.dedups += 1
                    continue

                # Step 4: build and append the row.
                row = self._build_jsonl_row(clip_path, manifest_row, geometry, ingested_at_utc, sha)
                fp.write(json.dumps(row, sort_keys=True) + "\n")
                seen_sha.add(sha)
                stats.written += 1

                if saves_since_flush >= 50:
                    save_progress(self.progress_path, state)
                    saves_since_flush = 0

                if idx % 1000 == 0:
                    self._log.info(
                        "progress: %d/%d (wrote=%d, dl-failed=%d, broken=%d, dedups=%d)",
                        idx,
                        total,
                        stats.written,
                        stats.skipped_download,
                        stats.skipped_broken,
                        stats.dedups,
                    )

        # Step 5: final flush.
        save_progress(self.progress_path, state)

        if total > 0:
            stats.attrition_pct = stats.skipped_download / total

        self._log.info(
            "wrote %d rows, skipped %d (download-failed), %d (broken-clip), %d dedups",
            stats.written,
            stats.skipped_download,
            stats.skipped_broken,
            stats.dedups,
        )

        if stats.attrition_pct > self.attrition_warn_threshold:
            self._log.warning(
                "download attrition %.1f%% exceeds advisory threshold %.1f%% "
                "(check %s for failure reasons)",
                stats.attrition_pct * 100.0,
                self.attrition_warn_threshold * 100.0,
                self.progress_path,
            )

        return stats
