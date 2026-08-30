# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""HDR detection + codec-specific HDR encode flag dispatch.

Bucket #9 of the Phase A capability audit (Research-0054). HDR sources
have distinct color metadata — BT.2020 primaries, PQ (SMPTE-2084) or
HLG (ARIB STD-B67) transfer — and codecs expect that metadata back on
the encode side via codec-specific flag families. This module:

1. probes a video file with ``ffprobe -show_streams -of json`` and
   classifies the first video stream as PQ HDR / HLG HDR / SDR;
2. emits the codec-appropriate ffmpeg flag list per detected adapter
   (global color tags for every HDR-capable row, codec-private HDR SEI
   where FFmpeg exposes a stable flag family);
3. resolves a fork-local HDR VMAF model JSON if one is shipped, else
   returns ``None`` so callers fall back to the SDR model with a
   logged warning.

The detection is deliberately permissive — partial / missing color
metadata returns ``None`` (caller treats as SDR). Misclassifying SDR
as HDR is the dangerous failure mode (would inject PQ flags into a
gamma-2.4 encode); misclassifying HDR as SDR is recoverable (encode
proceeds without HDR signaling, scores trend low, user re-runs with
``--force-hdr``).

See :doc:`docs/adr/0300-vmaf-tune-hdr-aware.md`.
"""

from __future__ import annotations

import dataclasses
import json
import logging
import shutil
import subprocess
from collections.abc import Callable
from pathlib import Path
from typing import Any

_LOG = logging.getLogger(__name__)


# Transfer-characteristic strings ffprobe emits. PQ = SMPTE ST 2084
# (Dolby/HDR10 OETF); HLG = ARIB STD-B67 (BBC/NHK broadcast HDR).
_PQ_TRANSFERS = frozenset({"smpte2084", "smpte-st-2084", "smpte_st_2084"})
_HLG_TRANSFERS = frozenset({"arib-std-b67", "arib_std_b67", "aribstdb67", "hlg"})

# BT.2020 primaries strings ffprobe emits. The 'bt2020' bucket folds
# both ncl (non-constant luminance, the common case) and cl (constant
# luminance, very rare in delivery).
_BT2020_PRIMARIES = frozenset({"bt2020", "bt2020nc", "bt2020-ncl", "bt2020c", "bt2020-cl"})


@dataclasses.dataclass(frozen=True)
class HdrInfo:
    """Detected HDR signaling on a video stream.

    ``transfer`` is the canonical fork-local identifier (``"pq"`` or
    ``"hlg"``). ``primaries`` and ``matrix`` are the raw ffprobe
    strings; downstream encoders need them verbatim. ``master_display``
    and ``max_cll`` are the SEI-payload strings ffmpeg accepts via
    ``-color_*`` / ``-x265-params master-display=...``; populated only
    when ffprobe surfaces them via stream side-data.
    """

    transfer: str  # "pq" | "hlg"
    primaries: str
    matrix: str
    color_range: str
    pix_fmt: str
    master_display: str | None = None
    max_cll: str | None = None


def detect_hdr(
    video_path: Path,
    *,
    ffprobe_bin: str = "ffprobe",
    runner: Callable[..., Any] | None = None,
) -> HdrInfo | None:
    """Probe ``video_path`` and return :class:`HdrInfo` or ``None``.

    Returns ``None`` for SDR sources, missing files, ffprobe failure,
    or any classification ambiguity. The ``runner`` parameter mirrors
    the encode/score modules — tests inject a stub returning a fake
    ffprobe JSON payload.
    """
    if not video_path.exists():
        _LOG.debug("hdr-detect: %s does not exist", video_path)
        return None

    if runner is None:
        if shutil.which(ffprobe_bin) is None:
            _LOG.debug("hdr-detect: %s not on PATH", ffprobe_bin)
            return None
        runner = subprocess.run

    cmd = [
        ffprobe_bin,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_streams",
        "-show_entries",
        (
            "stream=color_transfer,color_primaries,color_space,color_range,pix_fmt:"
            "stream_side_data=side_data_type,red_x,red_y,green_x,green_y,blue_x,blue_y,"
            "white_point_x,white_point_y,min_luminance,max_luminance,max_content,max_average"
        ),
        "-of",
        "json",
        str(video_path),
    ]
    try:
        completed = runner(cmd, capture_output=True, text=True, check=False)
    except (OSError, FileNotFoundError) as exc:
        _LOG.debug("hdr-detect: ffprobe invocation failed: %s", exc)
        return None

    rc = int(getattr(completed, "returncode", 1))
    if rc != 0:
        _LOG.debug("hdr-detect: ffprobe returned %d", rc)
        return None

    try:
        payload = json.loads(getattr(completed, "stdout", "") or "{}")
    except json.JSONDecodeError:
        return None

    return _classify_payload(payload)


def _classify_payload(payload: dict[str, Any]) -> HdrInfo | None:
    """Pure helper — turns an ffprobe JSON payload into HdrInfo."""
    streams = payload.get("streams") or []
    if not streams:
        return None
    s = streams[0]

    transfer = (s.get("color_transfer") or "").lower()
    primaries = (s.get("color_primaries") or "").lower()
    matrix = (s.get("color_space") or "").lower()
    color_range = (s.get("color_range") or "").lower()
    pix_fmt = (s.get("pix_fmt") or "").lower()

    if transfer in _PQ_TRANSFERS:
        canonical = "pq"
    elif transfer in _HLG_TRANSFERS:
        canonical = "hlg"
    else:
        return None

    # PQ/HLG transfer without BT.2020 primaries is malformed; treat as
    # SDR so we don't inject mismatched signaling. Users with edge-case
    # sources can bypass via --force-hdr.
    if primaries not in _BT2020_PRIMARIES:
        _LOG.warning(
            "hdr-detect: %s transfer with non-bt2020 primaries %r; treating as SDR",
            canonical,
            primaries,
        )
        return None

    master_display, max_cll = _extract_mastering(s.get("side_data_list") or [])
    return HdrInfo(
        transfer=canonical,
        primaries=primaries,
        matrix=matrix or "bt2020nc",
        color_range=color_range or "tv",
        pix_fmt=pix_fmt or "yuv420p10le",
        master_display=master_display,
        max_cll=max_cll,
    )


def _extract_mastering(side_data: list[dict[str, Any]]) -> tuple[str | None, str | None]:
    """Pull mastering-display + content-light SEI from side data.

    Returns ``(master_display, max_cll)`` strings in the format
    x265 / SVT-AV1 expect, or ``(None, None)`` if not present. The
    coordinate scale follows the H.265/HEVC convention: chromaticity
    in 0.00002 units, luminance in 0.0001 cd/m^2 units.
    """
    md_str: str | None = None
    cll_str: str | None = None
    for sd in side_data:
        kind = (sd.get("side_data_type") or "").lower()
        if "mastering display" in kind:
            md_str = _format_master_display(sd)
        elif "content light" in kind:
            mc = sd.get("max_content")
            ma = sd.get("max_average")
            if mc is not None and ma is not None:
                cll_str = f"{int(mc)},{int(ma)}"
    return md_str, cll_str


def _format_master_display(sd: dict[str, Any]) -> str | None:
    """x265 ``master-display`` format: ``G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)``.

    ffprobe surfaces fractions as ``"<num>/<den>"`` strings. Encoders
    expect chroma in 0.00002 units, luminance in 0.0001 cd/m^2.
    """
    keys = (
        "green_x",
        "green_y",
        "blue_x",
        "blue_y",
        "red_x",
        "red_y",
        "white_point_x",
        "white_point_y",
    )
    coords: list[int] = []
    for k in keys:
        v = sd.get(k)
        if v is None:
            return None
        coords.append(_frac_to_unit(v, scale=50000))
    lmax = sd.get("max_luminance")
    lmin = sd.get("min_luminance")
    if lmax is None or lmin is None:
        return None
    lmax_u = _frac_to_unit(lmax, scale=10000)
    lmin_u = _frac_to_unit(lmin, scale=10000)
    return (
        f"G({coords[0]},{coords[1]})"
        f"B({coords[2]},{coords[3]})"
        f"R({coords[4]},{coords[5]})"
        f"WP({coords[6]},{coords[7]})"
        f"L({lmax_u},{lmin_u})"
    )


def _frac_to_unit(value: Any, *, scale: int) -> int:
    """Convert ffprobe's ``"num/den"`` fraction (or float / int) to scaled int."""
    if isinstance(value, (int, float)):
        return round(float(value) * scale)
    text = str(value)
    if "/" in text:
        num, den = text.split("/", 1)
        try:
            return round((float(num) / float(den)) * scale)
        except (ValueError, ZeroDivisionError):
            return 0
    try:
        return round(float(text) * scale)
    except ValueError:
        return 0


# ---------------------------------------------------------------------------
# Codec-side flag dispatch
# ---------------------------------------------------------------------------


def hdr_codec_args(encoder: str, info: HdrInfo) -> tuple[str, ...]:
    """Return ffmpeg argv tail to inject HDR signaling for ``encoder``.

    Empty tuple = encoder has no HDR-specific flags or HDR is not yet
    wired for this codec adapter. Callers append the result to the
    base ffmpeg command after the ``-c:v`` argument.

    The dispatch table is the central contract. Codec adapters own
    quality/preset argv; this function owns HDR color signaling so the
    corpus and auto planner stay consistent across encoders.
    """
    dispatch = {
        "libaom-av1": _hdr_args_global_only,
        "libx264": _hdr_args_x264,
        "libx265": _hdr_args_x265,
        "libsvtav1": _hdr_args_svtav1,
        "av1_nvenc": _hdr_args_av1_10bit_global,
        "hevc_nvenc": _hdr_args_nvenc_hevc,
        "hevc_qsv": _hdr_args_hevc_main10_global,
        "av1_qsv": _hdr_args_av1_10bit_global,
        "hevc_amf": _hdr_args_hevc_main10_global,
        "av1_amf": _hdr_args_av1_10bit_global,
        "hevc_videotoolbox": _hdr_args_hevc_main10_global,
        "libvvenc": _hdr_args_vvenc,
    }
    fn = dispatch.get(encoder)
    if fn is None:
        _LOG.warning("hdr-codec-args: no HDR dispatch for encoder %r; emitting empty", encoder)
        return ()
    return fn(info)


def _global_color_args(info: HdrInfo) -> list[str]:
    """ffmpeg-level ``-color_*`` flags every HDR-capable codec wants."""
    transfer = "smpte2084" if info.transfer == "pq" else "arib-std-b67"
    return [
        "-color_primaries",
        "bt2020",
        "-color_trc",
        transfer,
        "-colorspace",
        info.matrix or "bt2020nc",
        "-color_range",
        info.color_range or "tv",
    ]


def _hdr_args_global_only(info: HdrInfo) -> tuple[str, ...]:
    """Codec supports ffmpeg-level color tagging but no stable private HDR SEI knobs."""
    return tuple(_global_color_args(info))


def _hdr_args_hevc_main10_global(info: HdrInfo) -> tuple[str, ...]:
    """HEVC hardware-family baseline: force 10-bit output and global color tags."""
    args = ["-pix_fmt", "p010le", "-profile:v", "main10"]
    args.extend(_global_color_args(info))
    return tuple(args)


def _hdr_args_av1_10bit_global(info: HdrInfo) -> tuple[str, ...]:
    """AV1 hardware-family baseline: force 10-bit 4:2:0 output and global color tags."""
    args = ["-pix_fmt", "p010le"]
    args.extend(_global_color_args(info))
    return tuple(args)


def _hdr_args_x264(info: HdrInfo) -> tuple[str, ...]:
    """x264 has no in-stream HDR signaling beyond container-level color tags.

    We still emit the global ``-color_*`` ffmpeg flags so HDR sources
    keep their metadata in the muxed file even when x264 itself can't
    represent it. Real HDR encodes should switch to x265 / SVT-AV1.
    """
    return tuple(_global_color_args(info))


def _hdr_args_x265(info: HdrInfo) -> tuple[str, ...]:
    """x265: in-stream SEI via ``-x265-params``."""
    parts = [
        "colorprim=bt2020",
        f"transfer={'smpte2084' if info.transfer == 'pq' else 'arib-std-b67'}",
        "colormatrix=bt2020nc",
        "range=limited" if info.color_range != "pc" else "range=full",
    ]
    if info.master_display:
        parts.append(f"master-display={info.master_display}")
    if info.max_cll:
        parts.append(f"max-cll={info.max_cll}")
    if info.transfer == "pq":
        parts.append("hdr10-opt=1")
    args = list(_global_color_args(info))
    args.extend(["-x265-params", ":".join(parts)])
    return tuple(args)


def _hdr_args_svtav1(info: HdrInfo) -> tuple[str, ...]:
    """SVT-AV1: ``-svtav1-params color-primaries=9:transfer-characteristics=N:matrix-coefficients=9``.

    AV1 enum: 9 = BT.2020, 16 = SMPTE-2084 (PQ), 18 = ARIB-STD-B67 (HLG).
    """
    tc = 16 if info.transfer == "pq" else 18
    parts = [
        "color-primaries=9",
        f"transfer-characteristics={tc}",
        "matrix-coefficients=9",
        "color-range=0" if info.color_range != "pc" else "color-range=1",
    ]
    if info.master_display:
        parts.append(f"mastering-display={info.master_display}")
    if info.max_cll:
        parts.append(f"content-light={info.max_cll}")
    args = list(_global_color_args(info))
    args.extend(["-svtav1-params", ":".join(parts)])
    return tuple(args)


def _hdr_args_nvenc_hevc(info: HdrInfo) -> tuple[str, ...]:
    """NVENC HEVC: relies on ``-pix_fmt p010le -profile:v main10`` plus
    the ffmpeg-global ``-color_*`` flags for SEI propagation.
    """
    args = list(_hdr_args_hevc_main10_global(info))
    if info.master_display:
        args.extend(["-master_display", info.master_display])
    if info.max_cll:
        args.extend(["-max_cll", info.max_cll])
    return tuple(args)


def _hdr_args_vvenc(info: HdrInfo) -> tuple[str, ...]:
    """libvvenc / VVenC HDR signaling — uses the same ``-color_*`` shape
    as x265 for the global metadata; SEI options live behind the
    ``--vvenc-params`` family in newer ffmpeg builds.
    """
    return tuple(_global_color_args(info))


# ---------------------------------------------------------------------------
# HDR VMAF model resolution
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# HDR VMAF model registration
# ---------------------------------------------------------------------------
#
# Netflix maintains an HDR-trained VMAF model under the name
# ``vmaf_hdr_v0.6.1.json`` in research artefacts published outside
# their public ``model/`` tree (verified 2026-05-08 — the upstream
# ``Netflix/vmaf`` master ``model/`` directory ships no HDR-tagged
# JSON). The fork registers the *slot* here so that:
#
#   1. operators can drop a licensed copy of the JSON at the documented
#      path and the harness will pick it up automatically;
#   2. ``select_hdr_vmaf_model()`` always returns the SDR fallback
#      when the slot is empty, with a single-shot warning logged once
#      per process so noisy corpus runs don't drown the operator.
#
# When a real HDR model lands in the fork ``model/`` tree, this module
# is the only knob to flip — all callers go through
# ``select_hdr_vmaf_model``.

# Canonical filename for the HDR model JSON. Matches Netflix's research
# artefact name so a future port is a verbatim file drop.
HDR_MODEL_FILENAME = "vmaf_hdr_v0.6.1.json"

# Per-transfer dispatch: PQ and HLG share the HDR-trained model
# upstream (vmaf_hdr_v0.6.1 was trained on a mixed PQ + HLG corpus).
# Future Dolby-Vision-specific models would extend this map.
_HDR_MODEL_BY_TRANSFER: dict[str, str] = {
    "pq": HDR_MODEL_FILENAME,
    "hlg": HDR_MODEL_FILENAME,
}

# Module-scoped flag for the "HDR model not shipped" warning. We log
# once per process so repeated corpus rows don't spam stderr.
_HDR_MODEL_MISSING_WARNED = False


def hdr_model_name_for(transfer: str | None) -> str | None:
    """Return the HDR VMAF model JSON filename for a transfer string.

    ``transfer`` is the canonical fork-local identifier (``"pq"`` or
    ``"hlg"``); any other value (including ``None`` for SDR sources)
    returns ``None`` so the caller picks the SDR model.
    """
    if not transfer:
        return None
    return _HDR_MODEL_BY_TRANSFER.get(transfer.lower())


def select_hdr_vmaf_model(
    model_dir: Path | None = None,
    *,
    transfer: str | None = None,
) -> Path | None:
    """Return the HDR-trained VMAF model JSON if shipped, else ``None``.

    Looks under ``model/`` (or ``model_dir`` if provided — useful for
    tests). When ``transfer`` is set to ``"pq"`` / ``"hlg"`` the
    canonical filename ``vmaf_hdr_v0.6.1.json`` is preferred; falls
    back to a ``vmaf_hdr_*.json`` glob so operators can ship newer
    revisions without breaking the resolver.

    Returns ``None`` when no HDR model is shipped — callers fall back
    to the SDR model and a one-shot warning is logged at module scope.
    The transfer routing keeps the SDR fork-local default
    (``vmaf_v0.6.1``) intact for non-HDR sources.
    """
    base = model_dir if model_dir is not None else _default_model_dir()
    if base is None or not base.exists():
        _warn_hdr_model_missing_once(reason="model directory not found")
        return None

    # Prefer the canonical name when transfer routing is requested.
    canonical = hdr_model_name_for(transfer)
    if canonical is not None:
        canonical_path = base / canonical
        if canonical_path.is_file():
            return canonical_path

    candidates = sorted(base.glob("vmaf_hdr_*.json"))
    if not candidates:
        _warn_hdr_model_missing_once(
            reason=(
                "no vmaf_hdr_*.json shipped — Netflix's vmaf_hdr_v0.6.1.json "
                "is published outside the upstream model/ tree and awaits a "
                "fork-local license review (ADR-0300 follow-up)"
            )
        )
        return None
    return candidates[-1]


def _warn_hdr_model_missing_once(*, reason: str) -> None:
    """Emit a single-shot warning that the HDR model is unavailable.

    Idempotent — repeated calls within a process are no-ops. Tests that
    need to re-trigger the warning (or assert the no-warning path) can
    call :func:`reset_hdr_model_warning` from this module.
    """
    global _HDR_MODEL_MISSING_WARNED
    if _HDR_MODEL_MISSING_WARNED:
        return
    _HDR_MODEL_MISSING_WARNED = True
    _LOG.warning(
        "select_hdr_vmaf_model: HDR VMAF model unavailable (%s); "
        "falling back to SDR model. Drop %s into model/ when a "
        "licensed copy is available.",
        reason,
        HDR_MODEL_FILENAME,
    )


def reset_hdr_model_warning() -> None:
    """Test-only helper — clears the once-per-process warning flag."""
    global _HDR_MODEL_MISSING_WARNED
    _HDR_MODEL_MISSING_WARNED = False


def _default_model_dir() -> Path | None:
    """Locate the in-tree ``model/`` directory relative to this file.

    The package lives at ``tools/vmaf-tune/src/vmaftune/`` so the
    repository root is four parents up.
    """
    here = Path(__file__).resolve()
    candidate = here.parents[4] / "model"
    return candidate if candidate.is_dir() else None
