# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Encoder runtime-token parsing for ``vmaf-tune compare``.

Codec adapters describe the FFmpeg argv shape (``libsvtav1`` uses
``-c:v libsvtav1``). Runtime variants describe which FFmpeg binary backs
that adapter in one compare run. This keeps forks such as SVT-AV1-HDR
auditable without pretending they are a distinct FFmpeg encoder.
"""

from __future__ import annotations

import dataclasses
from collections.abc import Iterable, Sequence


@dataclasses.dataclass(frozen=True)
class EncoderRuntimeSpec:
    """Resolved encoder token used by compare/report rows.

    ``token`` is the user-facing label (for example
    ``libsvtav1@svt-av1-hdr``). ``adapter`` is the adapter-registry key
    used for argv construction (``libsvtav1``). ``ffmpeg_bin`` is the
    runtime bound to that one token.
    """

    token: str
    adapter: str
    variant: str
    ffmpeg_bin: str


def parse_encoder_runtime_token(token: str, *, ffmpeg_bin: str = "ffmpeg") -> EncoderRuntimeSpec:
    """Parse ``ADAPTER`` or ``ADAPTER@VARIANT`` into a runtime spec.

    The variant suffix is a display/provenance label only; it never
    changes the adapter key. ``libsvtav1@svt-av1-hdr`` therefore still
    routes through the ``libsvtav1`` adapter and FFmpeg's
    ``-c:v libsvtav1`` encoder.
    """
    raw = token.strip()
    if not raw:
        raise ValueError("empty encoder token")
    if raw.count("@") > 1:
        raise ValueError(f"invalid encoder token {token!r}: use ADAPTER or ADAPTER@VARIANT")

    if "@" in raw:
        adapter, variant = raw.split("@", 1)
        if not adapter or not variant:
            raise ValueError(f"invalid encoder token {token!r}: adapter and variant are required")
    else:
        adapter = raw
        variant = ""

    for part_name, part in (("adapter", adapter), ("variant", variant)):
        if not part:
            continue
        if any(ch.isspace() for ch in part):
            raise ValueError(f"invalid encoder token {token!r}: {part_name} contains whitespace")
        if "," in part or "=" in part:
            raise ValueError(f"invalid encoder token {token!r}: {part_name} contains ',' or '='")

    display = f"{adapter}@{variant}" if variant else adapter
    return EncoderRuntimeSpec(
        token=display,
        adapter=adapter,
        variant=variant,
        ffmpeg_bin=ffmpeg_bin,
    )


def resolve_encoder_runtime_specs(
    tokens: Sequence[str],
    *,
    ffmpeg_bin: str = "ffmpeg",
    encoder_ffmpeg_bins: Iterable[str] | None = None,
) -> tuple[EncoderRuntimeSpec, ...]:
    """Resolve compare encoder tokens and per-token FFmpeg bindings.

    ``encoder_ffmpeg_bins`` entries use ``TOKEN=PATH`` where ``TOKEN`` is
    an exact member of ``tokens`` after normalisation. Unknown binding
    keys are rejected so a typo cannot silently route a variant through
    the global FFmpeg binary.
    """
    specs = tuple(parse_encoder_runtime_token(token, ffmpeg_bin=ffmpeg_bin) for token in tokens)
    seen: set[str] = set()
    duplicates: list[str] = []
    for spec in specs:
        if spec.token in seen:
            duplicates.append(spec.token)
        else:
            seen.add(spec.token)
    if duplicates:
        raise ValueError(f"duplicate encoder token(s): {', '.join(duplicates)}")

    by_token = {spec.token: spec for spec in specs}
    overrides: dict[str, str] = {}
    for raw_binding in encoder_ffmpeg_bins or ():
        binding = raw_binding.strip()
        if "=" not in binding:
            raise ValueError(f"invalid --encoder-ffmpeg-bin {raw_binding!r}: expected ENCODER=PATH")
        raw_key, raw_path = binding.split("=", 1)
        key = parse_encoder_runtime_token(raw_key).token
        path = raw_path.strip()
        if not path:
            raise ValueError(
                f"invalid --encoder-ffmpeg-bin {raw_binding!r}: PATH must not be empty"
            )
        if key not in by_token:
            known = ", ".join(sorted(by_token))
            raise ValueError(
                f"invalid --encoder-ffmpeg-bin {raw_binding!r}: {key!r} is not in "
                f"--encoders ({known})"
            )
        overrides[key] = path

    return tuple(
        dataclasses.replace(spec, ffmpeg_bin=overrides.get(spec.token, spec.ffmpeg_bin))
        for spec in specs
    )


__all__ = [
    "EncoderRuntimeSpec",
    "parse_encoder_runtime_token",
    "resolve_encoder_runtime_specs",
]
