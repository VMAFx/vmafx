# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""FR-from-NR corpus adapter (ADR-0346).

Converts a no-reference (NR) corpus row — a distorted MP4 plus a
crowdworker MOS, no clean reference YUV — into one or more
full-reference (FR) corpus rows in the existing
:data:`vmaftune.CORPUS_ROW_KEYS` schema.

Pipeline (per NR input row)::

    1. ffprobe(source.mp4)            -> (W, H, pix_fmt, fps, duration_s)
    2. ffmpeg decode source.mp4       -> raw YUV "reference" (intermediate)
    3. for crf in crf_sweep:
           vmaftune.corpus.iter_rows  -> 1 FR row at this CRF
    4. cleanup raw YUV intermediate
    5. yield N FR rows                (N == len(crf_sweep))

The "reference" is the re-decoded upload, NOT a pristine master —
upload-side artifacts (YouTube transcode, capture-chain noise,
prior-encode blockiness) propagate into the reference and therefore
into every FR feature. ADR-0346 §Consequences §Negative documents
this caveat. Downstream consumers must treat NR-derived rows as
*delta-vs-already-distorted* signal, not delta-vs-pristine.

The adapter is harness-only and built around two seams that tests
mock out:

* ``probe_runner`` — replaces ``subprocess.run`` for the ffprobe call
  (returns parsed JSON dict).
* ``decode_runner`` — replaces ``subprocess.run`` for the ffmpeg
  decode-to-YUV call. The real implementation also receives the
  ``encode_runner`` / ``score_runner`` seams that
  :func:`vmaftune.corpus.iter_rows` already exposes for the FR sweep.

Because ``vmaftune.corpus.iter_rows`` already speaks the canonical
row schema, this adapter does **not** reshape rows; it only orchestrates
the decode-once / sweep-many step around it.
"""

from __future__ import annotations

import contextlib
import dataclasses
import json
import shlex
import subprocess
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

from .corpus import CorpusJob, CorpusOptions, iter_rows

# Default CRF sweep — 5 points spanning visually-lossless (18) to
# heavy compression (38) on libx264. Override per call. ADR-0346
# §Decision pins the default and the rationale.
DEFAULT_CRF_SWEEP: tuple[int, ...] = (18, 23, 28, 33, 38)

# Default preset for the FR sweep. Single-preset by default keeps the
# corpus-multiplier predictable (1 NR row -> len(crf_sweep) FR rows).
# Operators who want a (preset, crf) grid pass a ``cells`` tuple
# directly via ``NrToFrAdapter.run``.
DEFAULT_PRESET: str = "medium"

# Pix-fmt fallback when ffprobe surfaces an unparseable / empty value.
# Most YouTube-UGC uploads at K150K are yuv420p; this fallback keeps
# the pipeline running rather than aborting on the long tail.
_PIX_FMT_FALLBACK: str = "yuv420p"


@dataclasses.dataclass(frozen=True)
class NrSourceGeometry:
    """Geometry probed from an NR input MP4 via ffprobe.

    All fields populated; ``duration_s`` may be 0.0 if ffprobe could
    not determine duration (rare; clip will fall back to full-source
    encode behaviour in :func:`vmaftune.corpus.iter_rows`).
    """

    width: int
    height: int
    pix_fmt: str
    framerate: float
    duration_s: float


@dataclasses.dataclass(frozen=True)
class NrInputRow:
    """Minimal NR-corpus row shape this adapter consumes.

    Mirrors the (subset of) keys present in the K150K JSONL on disk
    at ``.workingdir2/konvid-150k/konvid_150k.jsonl``. Extra keys are
    ignored; missing required keys raise ``KeyError`` at build time.
    """

    src: Path
    mos: float | None = None
    extra: dict[str, Any] | None = None

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> NrInputRow:
        if "src" not in d:
            raise KeyError("NR input row missing required key 'src'")
        return cls(
            src=Path(d["src"]),
            mos=d.get("mos"),
            extra={k: v for k, v in d.items() if k not in {"src", "mos"}},
        )


# Subprocess-runner protocol (matches what ``corpus.iter_rows`` accepts).
RunnerFn = Callable[..., Any]


def _default_subprocess_runner(*args: Any, **kwargs: Any) -> Any:
    """Real subprocess runner used in production callers."""
    return subprocess.run(*args, **kwargs)


def probe_geometry(
    source: Path,
    *,
    ffprobe_bin: str = "ffprobe",
    runner: RunnerFn | None = None,
) -> NrSourceGeometry:
    """Probe ``source`` via ffprobe and return its raw-YUV geometry.

    Returns a fully-populated :class:`NrSourceGeometry`; callers are
    responsible for any sanity-checking on the values (e.g. zero
    duration, exotic pix_fmt). ``runner`` is the subprocess seam tests
    mock out — production callers leave it ``None``.
    """
    if runner is None:
        runner = _default_subprocess_runner
    cmd = [
        ffprobe_bin,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height,pix_fmt,r_frame_rate:format=duration",
        "-of",
        "json",
        str(source),
    ]
    completed = runner(cmd, capture_output=True, text=True, check=False)
    if getattr(completed, "returncode", 1) != 0:
        raise RuntimeError(
            f"ffprobe failed for {source}: rc={completed.returncode} "
            f"stderr={getattr(completed, 'stderr', '')!r}"
        )
    payload = json.loads(completed.stdout or "{}")
    streams = payload.get("streams") or [{}]
    s0 = streams[0] if streams else {}
    fmt = payload.get("format") or {}

    width = int(s0.get("width") or 0)
    height = int(s0.get("height") or 0)
    pix_fmt = str(s0.get("pix_fmt") or _PIX_FMT_FALLBACK) or _PIX_FMT_FALLBACK
    framerate = _parse_rational(s0.get("r_frame_rate"))
    try:
        duration_s = float(fmt.get("duration") or 0.0)
    except (TypeError, ValueError):
        duration_s = 0.0
    if width <= 0 or height <= 0:
        raise RuntimeError(f"ffprobe returned invalid geometry for {source}: w={width} h={height}")
    return NrSourceGeometry(
        width=width,
        height=height,
        pix_fmt=pix_fmt,
        framerate=framerate,
        duration_s=duration_s,
    )


def _parse_rational(value: Any) -> float:
    """Parse an ffprobe rational string ``"N/D"`` to float; tolerate noise."""
    if value is None:
        return 0.0
    if isinstance(value, (int, float)):
        return float(value)
    s = str(value)
    if "/" in s:
        num, _, den = s.partition("/")
        try:
            n = float(num)
            d = float(den) or 1.0
        except ValueError:
            return 0.0
        return n / d
    try:
        return float(s)
    except ValueError:
        return 0.0


def build_decode_command(
    source: Path,
    out_yuv: Path,
    geom: NrSourceGeometry,
    *,
    ffmpeg_bin: str = "ffmpeg",
) -> list[str]:
    """Return the ffmpeg argv that decodes ``source`` to a raw YUV file.

    The ``-pix_fmt`` of the output matches the probed source pix_fmt so
    the FR sweep encoder gets a frame-format identical to what came out
    of the upload's decoded stream — re-shape would silently introduce
    a color-conversion delta that downstream FR features would attribute
    to compression.
    """
    return [
        ffmpeg_bin,
        "-hide_banner",
        "-nostdin",
        "-y",
        "-i",
        str(source),
        "-f",
        "rawvideo",
        "-pix_fmt",
        geom.pix_fmt,
        str(out_yuv),
    ]


def decode_to_yuv(
    source: Path,
    out_yuv: Path,
    geom: NrSourceGeometry,
    *,
    ffmpeg_bin: str = "ffmpeg",
    runner: RunnerFn | None = None,
) -> None:
    """Decode ``source`` to ``out_yuv`` (raw YUV) at the probed geometry.

    Raises ``RuntimeError`` on a non-zero ffmpeg exit. ``out_yuv``'s
    parent directory must already exist; the adapter caller manages
    the scratch directory lifecycle.
    """
    if runner is None:
        runner = _default_subprocess_runner
    cmd = build_decode_command(source, out_yuv, geom, ffmpeg_bin=ffmpeg_bin)
    completed = runner(cmd, capture_output=True, text=True, check=False)
    if getattr(completed, "returncode", 1) != 0:
        raise RuntimeError(
            f"ffmpeg decode failed for {source}: "
            f"cmd={shlex.join(cmd)!r} "
            f"stderr={getattr(completed, 'stderr', '')!r}"
        )


@dataclasses.dataclass(frozen=True)
class NrToFrAdapter:
    """Decode-original-as-reference adapter (ADR-0346 §Decision).

    See module docstring for the pipeline. The dataclass holds *only*
    configuration; the actual orchestration lives in :meth:`run` and
    :meth:`run_many`.
    """

    crf_sweep: tuple[int, ...] = DEFAULT_CRF_SWEEP
    preset: str = DEFAULT_PRESET
    scratch_dir: Path = Path(".workingdir2/fr_from_nr_scratch")
    keep_intermediate_yuv: bool = False
    options: CorpusOptions = dataclasses.field(default_factory=CorpusOptions)

    def __post_init__(self) -> None:
        if not self.crf_sweep:
            raise ValueError("crf_sweep must be non-empty")
        for crf in self.crf_sweep:
            if not isinstance(crf, int):
                raise TypeError(f"crf_sweep entries must be int, got {crf!r}")
        if not isinstance(self.preset, str) or not self.preset:
            raise ValueError("preset must be a non-empty string")

    def _intermediate_yuv_path(self, source: Path) -> Path:
        # Use the source basename + '.yuv' under the scratch dir; the
        # adapter does not attempt cross-row deduplication (each NR row
        # is processed independently).
        stem = source.stem if source.stem else "input"
        return self.scratch_dir / f"{stem}.yuv"

    def run(
        self,
        nr_row: NrInputRow,
        *,
        probe_runner: RunnerFn | None = None,
        decode_runner: RunnerFn | None = None,
        encode_runner: RunnerFn | None = None,
        score_runner: RunnerFn | None = None,
    ) -> Iterator[dict[str, Any]]:
        """Yield FR corpus rows for one NR input row.

        ``probe_runner`` / ``decode_runner`` are the adapter's own
        subprocess seams. ``encode_runner`` / ``score_runner`` are
        forwarded into :func:`vmaftune.corpus.iter_rows` unchanged.
        Production callers leave all four ``None``.
        """
        geom = probe_geometry(
            nr_row.src,
            ffprobe_bin=self.options.ffprobe_bin,
            runner=probe_runner,
        )

        self.scratch_dir.mkdir(parents=True, exist_ok=True)
        yuv_path = self._intermediate_yuv_path(nr_row.src)
        decode_to_yuv(
            nr_row.src,
            yuv_path,
            geom,
            ffmpeg_bin=self.options.ffmpeg_bin,
            runner=decode_runner,
        )

        try:
            cells = tuple((self.preset, crf) for crf in self.crf_sweep)
            job = CorpusJob(
                source=yuv_path,
                width=geom.width,
                height=geom.height,
                pix_fmt=geom.pix_fmt,
                framerate=geom.framerate,
                duration_s=geom.duration_s,
                cells=cells,
            )
            for row in iter_rows(
                job,
                self.options,
                encode_runner=encode_runner,
                score_runner=score_runner,
            ):
                # Annotate provenance: this is an NR-derived FR row, the
                # "reference" is the decoded upload, not a pristine
                # master. Downstream loaders that gate on row provenance
                # read these keys.
                row["nr_source"] = str(nr_row.src)
                row["nr_mos"] = nr_row.mos
                row["fr_from_nr"] = True
                yield row
        finally:
            if not self.keep_intermediate_yuv:
                with contextlib.suppress(OSError):
                    yuv_path.unlink()

    def run_many(
        self,
        nr_rows: list[NrInputRow],
        *,
        probe_runner: RunnerFn | None = None,
        decode_runner: RunnerFn | None = None,
        encode_runner: RunnerFn | None = None,
        score_runner: RunnerFn | None = None,
    ) -> Iterator[dict[str, Any]]:
        """Yield FR rows for a list of NR inputs in deterministic order.

        Failures on any single NR row are surfaced as ``RuntimeError``
        with the offending source path; the adapter does **not**
        silently drop rows. Operators who want batch-tolerant behaviour
        wrap individual ``run()`` calls in their own try/except.
        """
        for nr_row in nr_rows:
            yield from self.run(
                nr_row,
                probe_runner=probe_runner,
                decode_runner=decode_runner,
                encode_runner=encode_runner,
                score_runner=score_runner,
            )


__all__ = [
    "DEFAULT_CRF_SWEEP",
    "DEFAULT_PRESET",
    "NrInputRow",
    "NrSourceGeometry",
    "NrToFrAdapter",
    "build_decode_command",
    "decode_to_yuv",
    "probe_geometry",
]
