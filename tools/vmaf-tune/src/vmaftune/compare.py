# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Codec-comparison mode (research-0061 Bucket #7).

Given a single source and one or more target VMAFs, run the per-codec
recommend predicate in parallel and emit a ranked report.

Two emission shapes coexist:

- **Single-target legacy** (schema v1): one row per codec at a single
  target VMAF — preserved verbatim for back-compat. Renders as the
  original bar+dot chart.
- **Multi-target sweep** (schema v2, ADR-0516): one row per
  ``(codec, target_vmaf)`` pair. Enables the rate-quality curve view
  (line per codec across multiple VMAF targets), pareto-frontier
  highlighting, and the summary table the report renderer now ships.
  Default for the `vmaf-tune compare` CLI when more than one target is
  passed via ``--target-vmafs``.

This module is intentionally a thin orchestration layer: the heavy
lifting lives in the per-codec recommend predicate, which is injected
via the ``predicate`` argument so tests stay subprocess-free and so
future codec adapters extend coverage without touching this file
(matches the codec-adapter discipline laid out in
``tools/vmaf-tune/AGENTS.md``).

The predicate signature is::

    predicate(codec: str, src: Path, target_vmaf: float) -> RecommendResult

The programmatic default predicate returns an ``ok=False`` row when
callers do not bind the source geometry required by Phase B bisect.
The CLI binds :func:`vmaftune.bisect.make_bisect_predicate` from its
``--width`` / ``--height`` / source-shape flags by default, and still
accepts a ``--predicate-module`` hook for custom backends or tests.
"""

from __future__ import annotations

import csv
import dataclasses
import io
import math
import time
from collections.abc import Callable, Iterable, Mapping, Sequence
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any

from . import __version__ as TOOL_VERSION
from .codec_adapters import known_codecs
from .encoder_runtime import parse_encoder_runtime_token
from .hw_devices import AUTO_VAAPI_DEVICE, resolve_vaapi_device
from .jsonio import dumps_strict

# Keys exposed to programmatic consumers. Mirrors the CORPUS_ROW_KEYS
# discipline in ``vmaftune/__init__.py``: bumping the list is a
# coordinated change.
COMPARE_ROW_KEYS: tuple[str, ...] = (
    "codec",
    "adapter",
    "runtime_variant",
    "ffmpeg_bin",
    "encoder_version",
    "best_crf",
    "bitrate_kbps",
    "encode_time_ms",
    "vmaf_score",
    "target_vmaf",
    "ok",
    "error",
)


@dataclasses.dataclass(frozen=True)
class RecommendResult:
    """One codec's best (CRF, bitrate, vmaf) tuple at a given target.

    ``ok=False`` carries a human-readable ``error`` string and leaves
    the numeric fields at sentinel values; the report renderer skips
    such rows in the ranking but still surfaces them in the table.

    ``bisect_samples`` (ADR-0530, schema-v2 additive) carries every
    encode+score probe the underlying target-VMAF bisect walked
    through before converging on ``best_crf``. Each entry is a dict
    with ``crf`` / ``bitrate_kbps`` / ``vmaf_score`` /
    ``encode_time_ms`` keys. The rate-quality chart consumes this list
    instead of the picked-CRF point to avoid the connect-the-dots
    artefact described in ADR-0530. Empty tuple means the predicate
    is not a bisect (older predicates / hand-written stubs).
    """

    codec: str
    best_crf: int
    bitrate_kbps: float
    encode_time_ms: float
    vmaf_score: float
    encoder_version: str = ""
    ok: bool = True
    error: str = ""
    bisect_samples: tuple[dict[str, Any], ...] = ()
    adapter: str = ""
    runtime_variant: str = ""
    ffmpeg_bin: str = ""

    def to_row(self, target_vmaf: float) -> dict[str, Any]:
        row: dict[str, Any] = {
            "codec": self.codec,
            "adapter": self.adapter,
            "runtime_variant": self.runtime_variant,
            "ffmpeg_bin": self.ffmpeg_bin,
            "encoder_version": self.encoder_version,
            "best_crf": self.best_crf,
            "bitrate_kbps": self.bitrate_kbps,
            "encode_time_ms": self.encode_time_ms,
            "vmaf_score": self.vmaf_score,
            "target_vmaf": target_vmaf,
            "ok": self.ok,
            "error": self.error,
        }
        # Additive (ADR-0530): only emit the key when the predicate
        # actually populated samples — keeps v1 CSV / single-target
        # JSON exporters unchanged and lets v2 readers detect "old
        # v2-without-samples" by the key's absence.
        if self.bisect_samples:
            row["bisect_samples"] = [dict(s) for s in self.bisect_samples]
        return row


@dataclasses.dataclass(frozen=True)
class ComparisonReport:
    """Result of ``compare_codecs`` — sorted ranking + metadata.

    ``rows`` are ordered ascending by ``bitrate_kbps`` for ``ok=True``
    rows; failed rows trail the ranking sorted by codec name. The raw
    ordering matters because the renderers preserve it.
    """

    src: str
    target_vmaf: float
    tool_version: str
    wall_time_ms: float
    rows: tuple[RecommendResult, ...]

    def best(self) -> RecommendResult | None:
        """Return the smallest-bitrate ok row, or ``None`` if all failed."""
        for r in self.rows:
            if r.ok:
                return r
        return None


PredicateFn = Callable[[str, Path, float], RecommendResult]
RowMetadataFn = Callable[[str], Mapping[str, str]]


def _apply_row_metadata(
    codec: str,
    result: RecommendResult,
    row_metadata: RowMetadataFn | None,
) -> RecommendResult:
    """Attach compare-row provenance for CLI runtime variants."""
    if row_metadata is None:
        return result
    meta = dict(row_metadata(codec))
    return dataclasses.replace(
        result,
        codec=codec,
        adapter=meta.get("adapter", result.adapter),
        runtime_variant=meta.get("runtime_variant", result.runtime_variant),
        ffmpeg_bin=meta.get("ffmpeg_bin", result.ffmpeg_bin),
    )


def _default_predicate(codec: str, src: Path, target_vmaf: float) -> RecommendResult:
    """Default predicate — points callers at :func:`bisect.make_bisect_predicate`.

    Phase B (target-VMAF bisect, see ``vmaftune.bisect``) is the
    production backend, but the bisect needs source geometry
    (``width``, ``height``, ``pix_fmt``, ``framerate``,
    ``duration_s``) that the bare ``(codec, src, target_vmaf)``
    predicate signature does not carry. Operators bind those once via
    ``make_bisect_predicate(...)`` and pass the resulting closure
    into ``compare_codecs(predicate=...)`` (or, in the CLI, via
    ``--predicate-module MODULE:CALLABLE``).

    The default predicate therefore returns ``ok=False`` with a
    well-formed row so callers see a clean report instead of a crash;
    the error string names the bisect entry-point so the next person
    who reads it has a one-step fix. The "Phase B pending" wording
    used to live here pre-ADR-0322 — the bisect itself ships in this
    PR, but compare's default predicate stays a pointer because the
    geometry-binding rule ("geometry is fixed before the predicate
    is built") would otherwise force compare to grow new arguments
    just to forward them through.
    """
    return RecommendResult(
        codec=codec,
        best_crf=-1,
        bitrate_kbps=float("nan"),
        encode_time_ms=float("nan"),
        vmaf_score=float("nan"),
        ok=False,
        error=(
            "no predicate wired. Use vmaftune.bisect.make_bisect_predicate("
            "target_vmaf, width=..., height=..., framerate=..., duration_s=...) "
            "and pass the result via compare_codecs(predicate=...) or "
            "--predicate-module MODULE:CALLABLE."
        ),
    )


def _rank(rows: Iterable[RecommendResult]) -> tuple[RecommendResult, ...]:
    """Sort: ok rows by ascending bitrate, failed rows trailing by codec."""
    materialized = list(rows)
    ok_rows = sorted(
        (r for r in materialized if r.ok),
        key=lambda r: (r.bitrate_kbps, r.codec),
    )
    fail_rows = sorted(
        (r for r in materialized if not r.ok),
        key=lambda r: r.codec,
    )
    return tuple([*ok_rows, *fail_rows])


def compare_codecs(
    src: Path,
    target_vmaf: float,
    encoders: Sequence[str],
    *,
    predicate: PredicateFn | None = None,
    parallel: bool = True,
    max_workers: int | None = None,
    pre_decoded_ref: "Path | None" = None,
    row_metadata: RowMetadataFn | None = None,
) -> ComparisonReport:
    """Run ``predicate`` per codec and rank by smallest bitrate.

    ``parallel=True`` dispatches each codec to a thread pool — every
    predicate call already shells out to ffmpeg / vmaf, so the GIL
    is not the bottleneck. ``max_workers`` defaults to ``len(encoders)``.

    ``pre_decoded_ref`` (ADR-0607): when set, every worker receives this
    already-decoded raw-YUV path as the ``src`` argument instead of the
    original container path. The report's ``src`` label still shows the
    original path so output is human-readable. Callers are responsible for
    creating and deleting the file; ``compare_codecs`` never touches it.
    This eliminates the per-worker reference re-decode that caused quadratic
    wall time with many concurrent workers (v14 BBB 1080p 9.7 h run).
    """
    if not encoders:
        raise ValueError("compare_codecs requires at least one encoder")
    pred = predicate if predicate is not None else _default_predicate
    src_path = Path(src)
    # ADR-0607: workers receive the pre-decoded YUV when available so each
    # bisect sees a raw-YUV src and skips the per-bisect reference decode.
    worker_src = Path(pre_decoded_ref) if pre_decoded_ref is not None else src_path
    t0 = time.monotonic()

    results: list[RecommendResult] = []
    if parallel and len(encoders) > 1:
        workers = max_workers if max_workers is not None else len(encoders)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futures = {
                pool.submit(pred, codec, worker_src, target_vmaf): codec for codec in encoders
            }
            for fut in as_completed(futures):
                codec = futures[fut]
                try:
                    results.append(_apply_row_metadata(codec, fut.result(), row_metadata))
                except Exception as exc:  # noqa: BLE001 — surface the error verbatim
                    results.append(
                        _apply_row_metadata(
                            codec,
                            RecommendResult(
                                codec=codec,
                                best_crf=-1,
                                bitrate_kbps=float("nan"),
                                encode_time_ms=float("nan"),
                                vmaf_score=float("nan"),
                                ok=False,
                                error=f"{type(exc).__name__}: {exc}",
                            ),
                            row_metadata,
                        )
                    )
    else:
        for codec in encoders:
            try:
                results.append(
                    _apply_row_metadata(codec, pred(codec, worker_src, target_vmaf), row_metadata)
                )
            except Exception as exc:  # noqa: BLE001
                results.append(
                    _apply_row_metadata(
                        codec,
                        RecommendResult(
                            codec=codec,
                            best_crf=-1,
                            bitrate_kbps=float("nan"),
                            encode_time_ms=float("nan"),
                            vmaf_score=float("nan"),
                            ok=False,
                            error=f"{type(exc).__name__}: {exc}",
                        ),
                        row_metadata,
                    )
                )

    return ComparisonReport(
        src=str(src_path),
        target_vmaf=float(target_vmaf),
        tool_version=TOOL_VERSION,
        wall_time_ms=(time.monotonic() - t0) * 1000.0,
        rows=_rank(results),
    )


def _emit_markdown(report: ComparisonReport) -> str:
    lines = [
        f"# Codec comparison — target VMAF {report.target_vmaf:g}",
        "",
        f"- Source: `{report.src}`",
        f"- Tool: `vmaf-tune {report.tool_version}`",
        f"- Wall time: {report.wall_time_ms:.1f} ms",
        "",
        "| Rank | Codec | Encoder | Best CRF | Bitrate (kbps) | "
        "Encode time (ms) | VMAF | Status |",
        "|---:|---|---|---:|---:|---:|---:|---|",
    ]
    rank = 0
    for r in report.rows:
        if r.ok:
            rank += 1
            rank_cell = str(rank)
            status = "ok"
        else:
            rank_cell = "—"
            status = f"fail: {r.error}"
        lines.append(
            f"| {rank_cell} | {r.codec} | {r.encoder_version or '—'} | "
            f"{r.best_crf if r.best_crf >= 0 else '—'} | "
            f"{r.bitrate_kbps:.1f} | {r.encode_time_ms:.1f} | "
            f"{r.vmaf_score:.2f} | {status} |"
        )
    best = report.best()
    if best is not None:
        lines.extend(
            [
                "",
                f"**Smallest file**: `{best.codec}` "
                f"at CRF {best.best_crf} → {best.bitrate_kbps:.1f} kbps "
                f"(VMAF {best.vmaf_score:.2f}).",
            ]
        )
    else:
        lines.extend(["", "**No codec succeeded.** See per-row error column."])
    return "\n".join(lines) + "\n"


def _emit_json(report: ComparisonReport) -> str:
    # NaN/±inf coercion + RFC 8259 strictness are handled by dumps_strict
    # (jsonio.py).  The private _nan_to_none helper that previously lived
    # here was an exact duplicate; removed per ADR-0988.
    payload = {
        "src": report.src,
        "target_vmaf": report.target_vmaf,
        "tool_version": report.tool_version,
        "wall_time_ms": report.wall_time_ms,
        "rows": [r.to_row(report.target_vmaf) for r in report.rows],
    }
    return dumps_strict(payload) + "\n"


def _emit_csv(report: ComparisonReport) -> str:
    buf = io.StringIO()
    # ``extrasaction="ignore"`` keeps CSV output stable when ``to_row``
    # learns optional columns over time (ADR-0530 added
    # ``bisect_samples``, which is structured and intentionally not
    # surfaced in the flat CSV view).
    writer = csv.DictWriter(buf, fieldnames=list(COMPARE_ROW_KEYS), extrasaction="ignore")
    writer.writeheader()
    for r in report.rows:
        writer.writerow(r.to_row(report.target_vmaf))
    return buf.getvalue()


_EMITTERS: dict[str, Callable[[ComparisonReport], str]] = {
    "markdown": _emit_markdown,
    "json": _emit_json,
    "csv": _emit_csv,
}


def emit_report(report: ComparisonReport, format: str = "markdown") -> str:
    """Render ``report`` as ``markdown`` / ``json`` / ``csv``."""
    fmt = format.lower()
    if fmt not in _EMITTERS:
        raise ValueError(f"unknown format {format!r}; expected one of {sorted(_EMITTERS)}")
    return _EMITTERS[fmt](report)


def supported_formats() -> tuple[str, ...]:
    return tuple(sorted(_EMITTERS))


# The canonical default codec list for the CLI. We only advertise codecs
# whose adapters actually live in ``codec_adapters/`` — adding x265 /
# SVT-AV1 / libaom / libvvenc later auto-expands this set.
def default_encoders() -> tuple[str, ...]:
    return known_codecs()


# ---------------------------------------------------------------------------
# Multi-target sweep (ADR-0516 / schema v2)
# ---------------------------------------------------------------------------

# Schema version stamped onto every JSON payload from `compare_codecs_sweep`.
# Renderers and downstream tools use this to pick the right ingester:
#  - v1 (legacy single-target): no ``schema_version`` key, no
#    ``target_vmafs`` key. Renders as the original bar+dot chart.
#  - v2 (multi-target sweep): ``schema_version=2``, ``target_vmafs=[...]``,
#    rows keyed by ``(codec, target_vmaf)``. Renders as the rate-quality
#    line chart with pareto-frontier highlight + summary table.
SCHEMA_VERSION_V1: int = 1
SCHEMA_VERSION_V2: int = 2


# Default CPU encoder set for `vmaf-tune compare` when ``--encoders`` is
# omitted. Hardware encoders (NVENC / QSV / AMF / VideoToolbox) are
# probed at runtime and skipped with a row-level error when absent
# rather than silently dropped from the comparison.
DEFAULT_CPU_ENCODERS: tuple[str, ...] = (
    "libx265",
    "libsvtav1",
)

# Hardware encoders the CLI accepts. Availability is probed via
# ``ffmpeg -encoders``; the per-encoder dummy-encode probe in
# :func:`probe_encoder_available` catches the "encoder present but no
# compatible GPU at runtime" case.
HARDWARE_ENCODERS: tuple[str, ...] = (
    "h264_nvenc",
    "hevc_nvenc",
    "av1_nvenc",
    "h264_qsv",
    "hevc_qsv",
    "av1_qsv",
    "h264_amf",
    "hevc_amf",
    "av1_amf",
)

# QSV encoder names — require VAAPI/QSV device-init flags and an
# ``hwupload`` filter before the encoder (ADR-0601 Bug V14-B).
_QSV_ENCODERS: frozenset[str] = frozenset({"h264_qsv", "hevc_qsv", "av1_qsv"})

# Default VA-API render-node selector used for QSV device initialisation.
# ``auto`` resolves to the first Intel render node in /sys/class/drm and
# falls back to /dev/dri/renderD128 only when no Intel node is discoverable.
_DEFAULT_VAAPI_DEVICE: str = AUTO_VAAPI_DEVICE

# Public alias — callers and tests can use this without importing the
# private name.
DEFAULT_VAAPI_DEVICE: str = _DEFAULT_VAAPI_DEVICE


def _hw_init_args_for_encoder(
    encoder: str,
    vaapi_device: str = _DEFAULT_VAAPI_DEVICE,
) -> list[str]:
    """Return the pre-input FFmpeg argv for hardware-device initialisation.

    NVENC / AMF: no device-init flags needed on Linux (the NVIDIA driver
    and the AMF runtime are discovered automatically). Returns ``[]``.

    QSV: FFmpeg's QSV bridge requires three device-init flags before the
    first ``-i`` input. The chain initialises a VA-API context, derives a
    QSV context from it, and nominates the VA-API device as the filter-
    hardware device so ``hwupload`` can push frames to the encoder::

        -init_hw_device vaapi=va:<device>
        -init_hw_device qsv=qsv_dev@va
        -filter_hw_device va

    Without these flags ``ffmpeg -c:v h264_qsv`` fails with
    ``-22 Invalid argument`` even when the Intel GPU driver is installed.

    The caller is also responsible for inserting
    ``-vf format=nv12,hwupload=extra_hw_frames=64`` before ``-c:v`` to
    push system-memory frames into QSV-mapped surfaces.

    See ADR-0601.
    """
    if encoder in _QSV_ENCODERS:
        resolved_vaapi_device = resolve_vaapi_device(vaapi_device)
        return [
            "-init_hw_device",
            f"vaapi=va:{resolved_vaapi_device}",
            "-init_hw_device",
            "qsv=qsv_dev@va",
            "-filter_hw_device",
            "va",
        ]
    # NVENC and AMF: no pre-input device-init required.
    return []


_PROBE_ERROR_HINTS: tuple[str, ...] = (
    "libamfrt64",
    "amf_not_supported",
    "failed to create hardware device context",
    "error creating a mfx session",
    "failed to initialise vaapi connection",
    "failed to create a vaapi device",
    "no capable devices",
    "cannot load",
    "permission denied",
)


def _probe_error_line(output: str) -> str:
    """Pick the most actionable line from an FFmpeg probe failure."""
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    for line in reversed(lines):
        lowered = line.lower()
        if any(hint in lowered for hint in _PROBE_ERROR_HINTS):
            return line
    return lines[-1] if lines else "no stderr"


@dataclasses.dataclass(frozen=True)
class SweepReport:
    """Result of :func:`compare_codecs_sweep` — multi-target sweep.

    ``rows`` are ordered ascending by ``(codec, target_vmaf)``. The
    renderer derives the per-codec line and the pareto frontier from
    this canonical ordering.
    """

    src: str
    target_vmafs: tuple[float, ...]
    tool_version: str
    wall_time_ms: float
    rows: tuple[RecommendResult, ...]
    # Parallel to ``rows`` — each entry is the ``target_vmaf`` the row
    # was produced at. Frozen-dataclass means the predicate's
    # RecommendResult does not need a target_vmaf field.
    row_targets: tuple[float, ...]

    def rows_with_targets(self) -> tuple[tuple[float, RecommendResult], ...]:
        return tuple(zip(self.row_targets, self.rows, strict=True))

    def best_for_target(self, target_vmaf: float) -> RecommendResult | None:
        """Smallest-bitrate ok row at ``target_vmaf``, or ``None``."""
        candidates = [
            r
            for t, r in self.rows_with_targets()
            if r.ok and math.isclose(t, target_vmaf, rel_tol=1e-6, abs_tol=1e-6)
        ]
        if not candidates:
            return None
        return min(candidates, key=lambda r: r.bitrate_kbps)


def probe_encoder_available(
    encoder: str,
    *,
    ffmpeg_bin: str = "ffmpeg",
    runner: Callable[..., Any] | None = None,
    vaapi_device: str = _DEFAULT_VAAPI_DEVICE,
) -> tuple[bool, str]:
    """Return ``(available, reason)`` for ``encoder`` against ``ffmpeg``.

    Two-stage probe:

    1. Run ``ffmpeg -hide_banner -encoders`` and grep for ``encoder``.
       Catches "encoder not compiled into this ffmpeg build" (the
       libvpx-vp9 case in slim images, the libvvenc case in the dev
       container, all hardware encoders on hosts without the SDK).
    2. For hardware encoders, run a 1-frame ``lavfi nullsrc`` dummy
       encode. Catches "encoder present but no compatible GPU runtime"
       (nvenc with no NVIDIA driver; qsv with no Intel iGPU; amf with
       no AMD GPU).

    ``vaapi_device`` selects the VA-API render node used for QSV device
    initialisation (ADR-0601). Ignored for NVENC and AMF.

    The ``runner`` hook keeps the helper test-friendly. Returns
    ``(True, "")`` when the encoder is usable.
    """
    import shlex
    import subprocess

    encoder_spec = parse_encoder_runtime_token(encoder, ffmpeg_bin=ffmpeg_bin)
    adapter_encoder = encoder_spec.adapter

    def _default_runner(argv: Sequence[str], timeout: float = 30.0):
        return subprocess.run(  # noqa: S603 — argv is internal, no shell
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )

    run = runner if runner is not None else _default_runner

    # Stage 1: encoder list. ``ffmpeg -encoders`` prints one line per
    # encoder with the encoder name in the second column.
    try:
        listing = run([ffmpeg_bin, "-hide_banner", "-encoders"])
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return False, f"ffmpeg unavailable for encoder probe: {exc}"

    # ``run()``'s subprocess.CompletedProcess.stdout is bytes by default.
    out = getattr(listing, "stdout", b"") or b""
    if isinstance(out, bytes):
        out_str = out.decode("utf-8", errors="replace")
    else:
        out_str = str(out)
    if not _encoder_listed(out_str, adapter_encoder):
        return False, (
            f"hardware encoder not available: {adapter_encoder} not compiled into ffmpeg"
        )

    # Stage 2 (hardware only): dummy 1-frame encode. The probe argv
    # uses lavfi ``nullsrc`` so it doesn't depend on any input file
    # being on disk. ``-f null -`` discards the output container so
    # we don't pay for muxer setup.
    #
    # ADR-0601 Bug V14-A: use 320x240 at 24 fps. Hardware encoders
    # (NVENC, QSV, AMF) reject 64x64 with -22 (EINVAL) because their
    # fixed-function encode blocks enforce a minimum resolution
    # (NVENC: 145x49; QSV: 128x96). 320x240 clears every known minimum.
    if adapter_encoder in HARDWARE_ENCODERS:
        # ADR-0601 Bug V14-B: QSV requires hardware-device init flags
        # before the input and an hwupload filter before the encoder.
        # NVENC and AMF need no pre-input device-init.
        pre_input_args = _hw_init_args_for_encoder(adapter_encoder, vaapi_device)
        argv = [
            ffmpeg_bin,
            "-hide_banner",
            "-loglevel",
            "error",
            *pre_input_args,
            "-f",
            "lavfi",
            "-i",
            "nullsrc=size=320x240:rate=24:duration=0.5",
            "-frames:v",
            "1",
        ]
        if adapter_encoder in _QSV_ENCODERS:
            argv += ["-vf", "format=nv12,hwupload=extra_hw_frames=64"]
        argv += ["-c:v", adapter_encoder, "-f", "null", "-"]
        try:
            probe = run(argv)
        except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
            return False, (f"hardware encoder not available: {adapter_encoder} probe failed: {exc}")
        rc = getattr(probe, "returncode", -1)
        if rc != 0:
            tail = getattr(probe, "stdout", b"") or b""
            tail_str = (
                tail.decode("utf-8", errors="replace") if isinstance(tail, bytes) else str(tail)
            )
            last_line = _probe_error_line(tail_str)
            return False, (
                f"hardware encoder not available: {adapter_encoder} dummy encode failed "
                f"(argv={shlex.join(argv)!r}): {last_line}"
            )

    return True, ""


def _encoder_listed(ffmpeg_encoders_stdout: str, encoder: str) -> bool:
    """True iff ``ffmpeg -encoders`` output advertises ``encoder``.

    Each non-header line has the form ``" V..... NAME    description"``;
    the encoder name appears as a whitespace-separated token in column 2.
    Token-match avoids "libx264" matching "libx264rgb" or "h264_nvenc"
    matching "h264_v4l2m2m" (substring matches false-positive there).
    """
    for raw in ffmpeg_encoders_stdout.splitlines():
        # Skip header / separator lines.
        if "------" in raw or not raw.strip():
            continue
        tokens = raw.split()
        # Encoder lines have flags as first token (e.g. ``V.....``).
        if len(tokens) >= 2 and tokens[1] == encoder:
            return True
    return False


def compare_codecs_sweep(
    src: Path,
    target_vmafs: Sequence[float],
    encoders: Sequence[str],
    *,
    predicate: PredicateFn | None = None,
    parallel: bool = True,
    max_workers: int | None = None,
    availability_probe: Callable[[str], tuple[bool, str]] | None = None,
    pre_decoded_ref: "Path | None" = None,
    row_metadata: RowMetadataFn | None = None,
) -> SweepReport:
    """Run ``predicate`` across the cross-product of codecs x targets.

    Encoders deemed unavailable by ``availability_probe`` get an
    ``ok=False`` row per target with a stable ``hardware encoder not
    available`` error string so the report renderer can flag them
    visually without short-circuiting the whole sweep.

    The cross-product is dispatched flat to the thread pool — one
    future per ``(codec, target_vmaf)`` pair — so a 4-codec x 4-target
    sweep saturates the pool across 16 independent bisects.

    ``pre_decoded_ref`` (ADR-0607): when set, every worker receives this
    already-decoded raw-YUV path as the ``src`` argument instead of the
    original container path. The report's ``src`` label still shows the
    original path so output is human-readable. Callers are responsible for
    creating and deleting the file; ``compare_codecs_sweep`` never touches it.
    This eliminates per-worker reference re-decodes that caused quadratic
    wall time with many concurrent workers (v14 BBB 1080p 9.7 h run).
    """
    if not encoders:
        raise ValueError("compare_codecs_sweep requires at least one encoder")
    if not target_vmafs:
        raise ValueError("compare_codecs_sweep requires at least one target VMAF")

    pred = predicate if predicate is not None else _default_predicate
    src_path = Path(src)
    # ADR-0607: workers receive the pre-decoded YUV when available so each
    # bisect sees a raw-YUV src and skips the per-bisect reference decode.
    worker_src = Path(pre_decoded_ref) if pre_decoded_ref is not None else src_path
    t0 = time.monotonic()

    # Probe each encoder once up front. Cache the per-encoder verdict
    # so we don't pay the probe cost N times across target sweeps.
    probe = availability_probe if availability_probe is not None else _no_probe
    availability: dict[str, tuple[bool, str]] = {}
    for codec in encoders:
        try:
            availability[codec] = probe(codec)
        except Exception as exc:  # noqa: BLE001 — probe is best-effort
            availability[codec] = (False, f"hardware encoder not available: probe crashed: {exc}")

    # Build the work list, substituting unavailable rows before dispatch.
    pairs: list[tuple[str, float]] = [(c, float(t)) for c in encoders for t in target_vmafs]
    results: list[RecommendResult] = [None] * len(pairs)  # type: ignore[list-item]
    target_track: list[float] = [t for _, t in pairs]

    dispatch_pairs: list[tuple[int, str, float]] = []
    for i, (codec, target) in enumerate(pairs):
        ok, reason = availability[codec]
        if not ok:
            results[i] = _apply_row_metadata(
                codec,
                RecommendResult(
                    codec=codec,
                    best_crf=-1,
                    bitrate_kbps=float("nan"),
                    encode_time_ms=float("nan"),
                    vmaf_score=float("nan"),
                    ok=False,
                    error=reason,
                ),
                row_metadata,
            )
        else:
            dispatch_pairs.append((i, codec, target))

    if parallel and len(dispatch_pairs) > 1:
        workers = max_workers if max_workers is not None else len(dispatch_pairs)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futures = {
                pool.submit(pred, codec, worker_src, target): (i, codec)
                for i, codec, target in dispatch_pairs
            }
            for fut in as_completed(futures):
                i, codec = futures[fut]
                try:
                    results[i] = _apply_row_metadata(codec, fut.result(), row_metadata)
                except Exception as exc:  # noqa: BLE001
                    results[i] = _apply_row_metadata(
                        codec,
                        RecommendResult(
                            codec=codec,
                            best_crf=-1,
                            bitrate_kbps=float("nan"),
                            encode_time_ms=float("nan"),
                            vmaf_score=float("nan"),
                            ok=False,
                            error=f"{type(exc).__name__}: {exc}",
                        ),
                        row_metadata,
                    )
    else:
        for i, codec, target in dispatch_pairs:
            try:
                results[i] = _apply_row_metadata(
                    codec,
                    pred(codec, worker_src, target),
                    row_metadata,
                )
            except Exception as exc:  # noqa: BLE001
                results[i] = _apply_row_metadata(
                    codec,
                    RecommendResult(
                        codec=codec,
                        best_crf=-1,
                        bitrate_kbps=float("nan"),
                        encode_time_ms=float("nan"),
                        vmaf_score=float("nan"),
                        ok=False,
                        error=f"{type(exc).__name__}: {exc}",
                    ),
                    row_metadata,
                )

    return SweepReport(
        src=str(src_path),
        target_vmafs=tuple(float(t) for t in target_vmafs),
        tool_version=TOOL_VERSION,
        wall_time_ms=(time.monotonic() - t0) * 1000.0,
        rows=tuple(results),
        row_targets=tuple(target_track),
    )


def _no_probe(_codec: str) -> tuple[bool, str]:
    """Default availability probe — always reports ``available`` so unit
    tests that pass a fake predicate don't accidentally exercise
    ffmpeg. CLI callers wire in :func:`probe_encoder_available`."""
    return True, ""


def emit_sweep_json(report: SweepReport) -> str:
    """Render a :class:`SweepReport` as schema-v2 JSON."""
    payload = {
        "schema_version": SCHEMA_VERSION_V2,
        "src": report.src,
        "target_vmafs": list(report.target_vmafs),
        "tool_version": report.tool_version,
        "wall_time_ms": report.wall_time_ms,
        "rows": [
            r.to_row(target) for target, r in zip(report.row_targets, report.rows, strict=True)
        ],
    }
    return dumps_strict(payload) + "\n"


def emit_sweep_csv(report: SweepReport) -> str:
    buf = io.StringIO()
    # ``extrasaction="ignore"`` keeps CSV output stable when ``to_row``
    # learns optional columns over time (ADR-0530 added
    # ``bisect_samples``, which is structured and intentionally not
    # surfaced in the flat CSV view).
    writer = csv.DictWriter(buf, fieldnames=list(COMPARE_ROW_KEYS), extrasaction="ignore")
    writer.writeheader()
    for target, r in zip(report.row_targets, report.rows, strict=True):
        writer.writerow(r.to_row(target))
    return buf.getvalue()


def emit_sweep_markdown(report: SweepReport) -> str:
    """Render a :class:`SweepReport` as a markdown rate-quality table.

    The table is grouped per codec, with one row per (codec, target).
    The summary table at the foot gives the bitrate at each target per
    codec — useful for ad-hoc dashboards or commit-message snippets.
    """
    lines: list[str] = [
        "# Codec rate-quality sweep",
        "",
        f"- Source: `{report.src}`",
        f"- Tool: `vmaf-tune {report.tool_version}` (schema v{SCHEMA_VERSION_V2})",
        f"- Targets: {', '.join(f'{t:g}' for t in report.target_vmafs)}",
        f"- Wall time: {report.wall_time_ms:.1f} ms",
        "",
        "| Codec | Encoder | Target VMAF | Best CRF | Bitrate (kbps) | "
        "Encode time (ms) | VMAF achieved | Status |",
        "|---|---|---:|---:|---:|---:|---:|---|",
    ]
    for target, r in zip(report.row_targets, report.rows, strict=True):
        if r.ok:
            status = "ok"
        else:
            status = f"fail: {r.error}"
        lines.append(
            f"| {r.codec} | {r.encoder_version or '—'} | {target:g} | "
            f"{r.best_crf if r.best_crf >= 0 else '—'} | "
            f"{r.bitrate_kbps:.1f} | {r.encode_time_ms:.1f} | "
            f"{r.vmaf_score:.2f} | {status} |"
        )
    # Summary table: bitrate at each target per codec.
    by_codec: dict[str, dict[float, RecommendResult]] = {}
    for target, r in zip(report.row_targets, report.rows, strict=True):
        by_codec.setdefault(r.codec, {})[target] = r

    lines.extend(
        [
            "",
            "## Summary table",
            "",
            "| Codec | Encoder | "
            + " | ".join(f"@ VMAF {t:g}" for t in report.target_vmafs)
            + " | best preset |",
            "|---|---|" + "|".join("---:" for _ in report.target_vmafs) + "|---|",
        ]
    )
    for codec, per_target in by_codec.items():
        cells: list[str] = []
        first_row = next(iter(per_target.values()))
        for t in report.target_vmafs:
            r = per_target.get(t)
            if r is None or not r.ok:
                cells.append("—")
            else:
                cells.append(f"{r.bitrate_kbps:.0f} kbps")
        lines.append(
            f"| {codec} | {first_row.encoder_version or '—'} | "
            + " | ".join(cells)
            + " | adapter default |"
        )
    return "\n".join(lines) + "\n"


_SWEEP_EMITTERS: dict[str, Callable[[SweepReport], str]] = {
    "markdown": emit_sweep_markdown,
    "json": emit_sweep_json,
    "csv": emit_sweep_csv,
}


def emit_sweep_report(report: SweepReport, format: str = "markdown") -> str:
    fmt = format.lower()
    if fmt not in _SWEEP_EMITTERS:
        raise ValueError(f"unknown format {format!r}; expected one of {sorted(_SWEEP_EMITTERS)}")
    return _SWEEP_EMITTERS[fmt](report)


def detect_schema_version(payload: dict[str, Any]) -> int:
    """Return ``2`` if ``payload`` matches the v2 sweep schema, else ``1``.

    The v1 single-target JSON has no ``schema_version`` key and no
    ``target_vmafs`` list. The v2 sweep JSON carries both. The
    ``--compare-json`` ingester in :mod:`vmaftune.cli` uses this to
    pick the right ingestion path so a v1 file produced by the legacy
    CLI still renders the bar+dot chart, while a v2 file from the
    sweep CLI renders the rate-quality curve.
    """
    if int(payload.get("schema_version", SCHEMA_VERSION_V1)) >= SCHEMA_VERSION_V2:
        return SCHEMA_VERSION_V2
    if "target_vmafs" in payload:
        return SCHEMA_VERSION_V2
    return SCHEMA_VERSION_V1


__all__ = [
    "COMPARE_ROW_KEYS",
    "ComparisonReport",
    "DEFAULT_CPU_ENCODERS",
    "DEFAULT_VAAPI_DEVICE",
    "HARDWARE_ENCODERS",
    "PredicateFn",
    "RecommendResult",
    "SCHEMA_VERSION_V1",
    "SCHEMA_VERSION_V2",
    "SweepReport",
    "compare_codecs",
    "compare_codecs_sweep",
    "default_encoders",
    "detect_schema_version",
    "emit_report",
    "emit_sweep_csv",
    "emit_sweep_json",
    "emit_sweep_markdown",
    "emit_sweep_report",
    "probe_encoder_available",
    "supported_formats",
]
