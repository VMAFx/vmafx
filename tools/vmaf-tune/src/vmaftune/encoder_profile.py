# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Read vmaf-tune report encoder profiles and build concrete encodes."""

from __future__ import annotations

import html
import json
import math
import re
from pathlib import Path
from typing import Any

from .codec_adapters import get_adapter
from .encode import EncodeRequest
from .report import ENCODER_PROFILE_SCHEMA

_HTML_PRE_RE = re.compile(r"<pre>(?P<payload>.*?)</pre>", re.DOTALL | re.IGNORECASE)
_MD_JSON_RE = re.compile(r"```json\s*(?P<payload>.*?)\s*```", re.DOTALL | re.IGNORECASE)
_RAW_VIDEO_SUFFIXES = {".raw", ".yuv", ".rgb", ".gray"}


def _extract_profile(payload: dict[str, Any]) -> dict[str, Any]:
    profile = payload.get("encoder_profile")
    if isinstance(profile, dict):
        payload = profile
    if payload.get("schema") != ENCODER_PROFILE_SCHEMA:
        raise ValueError(
            f"unsupported encoder profile schema {payload.get('schema')!r}; "
            f"expected {ENCODER_PROFILE_SCHEMA!r}"
        )
    if not isinstance(payload.get("recommendations"), list):
        raise ValueError("encoder profile has no recommendations list")
    return payload


def _json_from_text(text: str, *, source: Path) -> dict[str, Any]:
    stripped = text.lstrip()
    if stripped.startswith("{"):
        payload: dict[str, Any] = json.loads(text)
        return payload
    if source.suffix.lower() in {".html", ".htm"}:
        match = _HTML_PRE_RE.search(text)
        if match is None:
            raise ValueError("HTML report does not contain a raw JSON <pre> block")
        payload = json.loads(html.unescape(match.group("payload")))
        return payload
    match = _MD_JSON_RE.search(text)
    if match is None:
        raise ValueError("report does not contain a fenced JSON payload")
    payload = json.loads(match.group("payload"))
    return payload


def load_profile_payload(path: Path) -> dict[str, Any]:
    """Load an encoder profile from report JSON, report HTML, or report Markdown."""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ValueError(f"cannot read profile {path}: {exc}") from exc
    try:
        payload = _json_from_text(text, source=path)
    except json.JSONDecodeError as exc:
        raise ValueError(f"cannot parse profile JSON from {path}: {exc}") from exc
    return _extract_profile(payload)


def _finite_float(value: Any, *, default: float = math.nan) -> float:
    try:
        out = float(value)
    except (TypeError, ValueError):
        return default
    if not math.isfinite(out):
        return default
    return out


def _default_preset(codec: str) -> str:
    try:
        adapter = get_adapter(codec)
    except KeyError:
        return "medium"
    presets: tuple[str, ...] = tuple(getattr(adapter, "presets", ()))
    if not presets:
        return "medium"
    if "medium" in presets:
        return "medium"
    return presets[len(presets) // 2]


def select_recommendation(
    profile: dict[str, Any],
    *,
    codec: str | None = None,
    target_vmaf: float | None = None,
    recommendation_index: int | None = None,
) -> dict[str, Any]:
    """Select one recommendation from an encoder profile.

    Defaults to the first Pareto-selected row with the lowest bitrate. Filters narrow
    the candidate set before the optional zero-based recommendation index is applied.
    """
    recs = [r for r in profile.get("recommendations", ()) if isinstance(r, dict)]
    if codec is not None:
        recs = [r for r in recs if r.get("codec") == codec]
    if target_vmaf is not None:
        recs = [
            r
            for r in recs
            if math.isclose(_finite_float(r.get("target_vmaf")), target_vmaf, abs_tol=1e-6)
        ]
    if not recs:
        raise ValueError("encoder profile has no recommendation matching the requested filters")

    recs.sort(
        key=lambda r: (
            not bool(r.get("selected_pareto")),
            _finite_float(r.get("bitrate_kbps"), default=float("inf")),
            str(r.get("codec", "")),
            _finite_float(r.get("target_vmaf"), default=float("inf")),
        )
    )
    if recommendation_index is not None:
        if recommendation_index < 0 or recommendation_index >= len(recs):
            raise ValueError(
                f"recommendation index {recommendation_index} outside filtered range 0..{len(recs) - 1}"
            )
        return recs[recommendation_index]
    return recs[0]


def infer_source_is_container(source: Path, source_kind: str = "auto") -> bool:
    """Return whether FFmpeg should auto-detect the source container."""
    if source_kind == "container":
        return True
    if source_kind == "raw":
        return False
    if source_kind != "auto":
        raise ValueError(f"unknown source kind {source_kind!r}")
    return source.suffix.lower() not in _RAW_VIDEO_SUFFIXES


def build_encode_request(
    profile: dict[str, Any],
    recommendation: dict[str, Any],
    *,
    output: Path,
    source_override: Path | None = None,
    preset_override: str | None = None,
    pix_fmt_override: str | None = None,
    framerate_override: float | None = None,
    width_override: int | None = None,
    height_override: int | None = None,
    duration_override: float | None = None,
    source_kind: str = "auto",
    sample_clip_seconds: float = 0.0,
    sample_clip_start_s: float = 0.0,
    extra_params: tuple[str, ...] = (),
) -> EncodeRequest:
    """Build an :class:`EncodeRequest` from one selected profile row."""
    _source_field = profile.get("source") or {}
    # Profiles written by older vmaf-tune versions may store "source" as a
    # plain path string rather than a metadata dict.  Normalise before use so
    # callers get an AttributeError-free dict regardless of the stored type.
    if isinstance(_source_field, str):
        source_meta: dict = {"path": _source_field}
    else:
        source_meta = dict(_source_field) if _source_field else {}
    run_meta = profile.get("run") or {}
    source = source_override or Path(str(source_meta.get("path") or ""))
    if not str(source):
        raise ValueError("profile has no source path; pass --src")

    codec = str(recommendation.get("codec") or "")
    if not codec:
        raise ValueError("selected recommendation has no codec")
    quality = int(recommendation.get("crf", recommendation.get("quality", -1)))
    if quality < 0:
        raise ValueError("selected recommendation has no usable CRF/quality")

    source_is_container = infer_source_is_container(source, source_kind=source_kind)
    width = int(width_override or source_meta.get("width") or 0)
    height = int(height_override or source_meta.get("height") or 0)
    framerate = float(framerate_override or source_meta.get("fps") or 0.0)
    pix_fmt = str(pix_fmt_override or run_meta.get("pix_fmt") or "yuv420p")
    if not source_is_container and (width <= 0 or height <= 0 or framerate <= 0):
        raise ValueError("raw sources require width, height, and framerate in profile or flags")

    preset = str(
        preset_override
        or recommendation.get("preset")
        or run_meta.get("preset")
        or _default_preset(codec)
    )
    if preset == "adapter default":
        preset = _default_preset(codec)

    duration = float(
        duration_override if duration_override is not None else source_meta.get("duration_s") or 0.0
    )
    return EncodeRequest(
        source=source,
        width=width,
        height=height,
        pix_fmt=pix_fmt,
        framerate=framerate,
        encoder=codec,
        preset=preset,
        crf=quality,
        output=output,
        extra_params=extra_params,
        sample_clip_seconds=float(sample_clip_seconds),
        sample_clip_start_s=float(sample_clip_start_s),
        source_is_container=source_is_container,
        duration_s=duration,
    )


__all__ = [
    "build_encode_request",
    "infer_source_is_container",
    "load_profile_payload",
    "select_recommendation",
]
