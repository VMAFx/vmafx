#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
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
        # Bounded by the file length: ``iter`` stops at the first empty read,
        # which a regular file guarantees after at most
        # ceil(size / _SHA_CHUNK_BYTES) iterations. No unbounded ``while True``.
        for chunk in iter(lambda: fh.read(_SHA_CHUNK_BYTES), b""):
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


def _ffprobe_command(clip_path: Path, ffprobe_bin: str) -> list[str]:
    """Build the ffprobe argv that reports the first video stream as JSON."""
    return [
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


def _run_ffprobe(
    cmd: list[str],
    clip_path: Path,
    runner: Callable[..., subprocess.CompletedProcess[str]],
    timeout_s: float,
) -> str | None:
    """Run ffprobe and return its stdout, or ``None`` when the probe failed."""
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
    return stdout


def _parse_ffprobe_payload(
    stdout: str, clip_path: Path
) -> tuple[dict[str, Any], dict[str, Any]] | None:
    """Return ``(first_video_stream, whole_payload)``, or ``None`` when unusable."""
    try:
        payload = json.loads(stdout)
    except json.JSONDecodeError as exc:
        _LOG.warning("ffprobe non-JSON output for %s: %s", clip_path.name, exc)
        return None

    streams = payload.get("streams") or []
    if not streams:
        _LOG.warning("ffprobe: no video streams in %s", clip_path.name)
        return None
    return streams[0], payload


def _first_positive_duration(stream: dict[str, Any], payload: dict[str, Any]) -> float:
    """Return the first positive ``duration``, preferring the stream over the container.

    Falls back to the last parsable value (and ultimately ``0.0``) when neither
    source reports a positive duration.
    """
    duration_s = 0.0
    for src in (stream, payload.get("format") or {}):
        d = src.get("duration")
        if d is None:
            continue
        try:
            duration_s = float(d)
        except (TypeError, ValueError):
            continue
        if duration_s > 0:
            break
    return duration_s


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
    stdout = _run_ffprobe(_ffprobe_command(clip_path, ffprobe_bin), clip_path, runner, timeout_s)
    if stdout is None:
        return None

    parsed = _parse_ffprobe_payload(stdout, clip_path)
    if parsed is None:
        return None
    stream, payload = parsed

    return {
        "width": int(stream.get("width", 0) or 0),
        "height": int(stream.get("height", 0) or 0),
        "framerate": _parse_framerate(
            stream.get("avg_frame_rate") or stream.get("r_frame_rate") or ""
        ),
        "duration_s": _first_positive_duration(stream, payload),
        "pix_fmt": str(stream.get("pix_fmt") or ""),
        "encoder_upstream": str(stream.get("codec_name") or ""),
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


#: Flush the resumable-progress JSON every N recorded state mutations. Small
#: enough that an interrupted multi-hour ingest loses at most this many
#: download decisions, large enough that the rewrite cost stays negligible.
_PROGRESS_FLUSH_EVERY: int = 50

#: Emit one aggregate progress line every N manifest rows.
_PROGRESS_LOG_EVERY: int = 1000


class _ProgressFlusher:
    """Batch resumable-progress writes into one flush per N mutations.

    ``record()`` counts a state mutation the caller has not persisted yet;
    ``flush_if_due()`` writes the whole state out and resets the counter once
    the batch is full. Keeping the counter in an object (rather than a local)
    lets :class:`CorpusIngestBase` split its ingest loop into helpers without
    changing when the progress file is actually written.
    """

    def __init__(self, path: Path, *, every: int = _PROGRESS_FLUSH_EVERY) -> None:
        self._path = path
        self._every = every
        self.pending = 0

    def record(self) -> None:
        """Count one progress-state mutation that is not yet on disk."""
        self.pending += 1

    def flush_if_due(self, state: dict[str, dict[str, Any]]) -> None:
        """Write ``state`` out when at least ``every`` mutations are pending."""
        if self.pending >= self._every:
            save_progress(self._path, state)
            self.pending = 0


# ---------------------------------------------------------------------------
# curl-backed downloader
# ---------------------------------------------------------------------------


def _curl_command(url: str, part: Path, curl_bin: str, timeout_s: int) -> list[str]:
    """Build the curl argv that fetches ``url`` into the ``.part`` staging file."""
    return [
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


def _run_curl(
    cmd: list[str],
    part: Path,
    runner: Callable[..., subprocess.CompletedProcess[str]],
    timeout_s: int,
) -> str:
    """Run curl; return an empty string on success or a short failure reason.

    The ``.part`` staging file is removed on every failure path so a retry
    never resumes from a truncated body.
    """
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
        return f"curl-spawn-timeout: exceeded {timeout_s}s+30s"
    except (FileNotFoundError, OSError) as exc:
        return f"curl-spawn-failed: {exc}"

    rc = getattr(proc, "returncode", 1)
    if rc != 0:
        with contextlib.suppress(OSError):
            part.unlink()
        stderr = (getattr(proc, "stderr", "") or "").strip()
        return f"curl-rc={rc}: {stderr[:200]}"

    if not part.is_file() or part.stat().st_size == 0:
        with contextlib.suppress(OSError):
            part.unlink()
        return "curl-empty-output"
    return ""


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

    reason = _run_curl(_curl_command(url, part, curl_bin, timeout_s), part, runner, timeout_s)
    if reason:
        return False, reason

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

    def _log_resume_state(self, state: dict[str, dict[str, Any]]) -> None:
        """Log how many clips a previous run already finished or gave up on."""
        if not state:
            return
        already_done = sum(1 for v in state.values() if v.get("state") == STATE_DONE)
        already_failed = sum(1 for v in state.values() if v.get("state") == STATE_FAILED)
        self._log.info(
            "resume: %d clips done, %d clips failed (from %s)",
            already_done,
            already_failed,
            self.progress_path,
        )

    def _collect_manifest_rows(self, clips_dir: Path) -> list[tuple[Path, dict[str, Any]]]:
        """Materialise the manifest rows, honouring the ``--max-rows`` cap."""
        rows: list[tuple[Path, dict[str, Any]]] = list(self.iter_source_rows(clips_dir))
        if self.max_rows is not None and len(rows) > self.max_rows:
            self._log.info(
                "capping manifest at max_rows=%d (full CSV had %d)",
                self.max_rows,
                len(rows),
            )
            rows = rows[: self.max_rows]
        return rows

    def _ensure_clip_available(
        self,
        clip_path: Path,
        manifest_row: dict[str, Any],
        state: dict[str, dict[str, Any]],
        stats: RunStats,
        flusher: _ProgressFlusher,
    ) -> bool:
        """Make sure ``clip_path`` is on disk, downloading it when it is not.

        Returns ``True`` when the clip can be probed, ``False`` when the row
        has to be skipped (download refused by the retry policy, or failed).
        """
        filename = clip_path.name
        if clip_path.is_file():
            if state.get(filename, {}).get("state") != STATE_DONE:
                mark_done(state, filename)
                flusher.record()
            return True

        if not should_attempt(state, filename, clip_path):
            stats.skipped_download += 1
            return False

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
            flusher.record()
            flusher.flush_if_due(state)
            return False

        mark_done(state, filename)
        flusher.record()
        return True

    def _probe_usable_geometry(self, clip_path: Path, stats: RunStats) -> dict[str, Any] | None:
        """Probe ``clip_path``; return ``None`` (and count it) when unusable."""
        geometry = probe_geometry(clip_path, ffprobe_bin=self.ffprobe_bin, runner=self.runner)
        if geometry is None:
            stats.skipped_broken += 1
            return None
        if geometry["width"] <= 0 or geometry["height"] <= 0:
            self._log.warning("ffprobe returned zero geometry for %s; skipping", clip_path.name)
            stats.skipped_broken += 1
            return None
        return geometry

    def _ingest_rows(
        self,
        rows: list[tuple[Path, dict[str, Any]]],
        state: dict[str, dict[str, Any]],
        seen_sha: set[str],
        ingested_at_utc: str,
    ) -> RunStats:
        """Download, probe, dedup and append every manifest row in ``rows``."""
        stats = RunStats()
        flusher = _ProgressFlusher(self.progress_path)
        total = len(rows)

        with self.output.open("a", encoding="utf-8") as fp:
            for idx, (clip_path, manifest_row) in enumerate(rows, start=1):
                # Step 1: ensure the clip is on disk.
                if not self._ensure_clip_available(clip_path, manifest_row, state, stats, flusher):
                    continue

                # Step 2: probe geometry.
                geometry = self._probe_usable_geometry(clip_path, stats)
                if geometry is None:
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

                flusher.flush_if_due(state)
                self._log_progress(idx, total, stats)

        return stats

    def _log_progress(self, idx: int, total: int, stats: RunStats) -> None:
        """Emit a periodic progress line every ``_PROGRESS_LOG_EVERY`` rows."""
        if idx % _PROGRESS_LOG_EVERY != 0:
            return
        self._log.info(
            "progress: %d/%d (wrote=%d, dl-failed=%d, broken=%d, dedups=%d)",
            idx,
            total,
            stats.written,
            stats.skipped_download,
            stats.skipped_broken,
            stats.dedups,
        )

    def _log_run_summary(self, stats: RunStats) -> None:
        """Log the aggregate counters and warn when download attrition is high."""
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
        self._log_resume_state(state)

        self.output.parent.mkdir(parents=True, exist_ok=True)
        seen_sha = read_sha_index(self.output)
        if seen_sha:
            self._log.info("resume: %d existing rows already in %s", len(seen_sha), self.output)

        ingested_at_utc = self.now_fn()
        rows = self._collect_manifest_rows(clips_dir)
        total = len(rows)

        stats = self._ingest_rows(rows, state, seen_sha, ingested_at_utc)

        # Step 5: final flush.
        save_progress(self.progress_path, state)

        if total > 0:
            stats.attrition_pct = stats.skipped_download / total

        self._log_run_summary(stats)
        return stats
