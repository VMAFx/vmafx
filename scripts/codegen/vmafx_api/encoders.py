# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The encoder registry, api/vmafx-encoders.toml (ADR-3388).

One table of every video encoder VMAFx knows about, implemented or not:
`[[encoders]]` (one row per encoder, keyed by the ADR-0644 adapter token) and
`[[variants]]` (one row per `ADAPTER@VARIANT`: a fork build or a tuning
profile of an encoder row). A row with `surfaces` is one the Go `pkg/encoder`
or the Python vmaf-tune adapters run; it carries an `[encoders.adapter]` table
with the quality knob, windows and presets both read. Every other row is a
planned, listed or decision-gated gap, with its licence class and the date its
upstream state was last checked.
"""

from __future__ import annotations

import re
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .entries import Entry, need
from .model import DefinitionError

DEFINITION = Path("api/vmafx-encoders.toml")
FORMAT_VERSION = 1
MAX_ROWS = 512  # bound for every table (HISS-02)
MAX_PRESETS = 16
PAIR = 2  # a range is [low, high]
TOKEN = re.compile(r"[a-z0-9][a-z0-9_.+-]*")
DATE = re.compile(r"\d{4}-\d{2}-\d{2}")
FAMILIES = frozenset(
    {"av1", "av2", "hevc", "h264", "vvc", "vp9", "vp8", "evc", "avs3", "avs2",
     "prores", "dnxhd", "ffv1", "jpeg-xs", "jpeg2000", "texture", "mpeg2",
     "legacy", "neural", "lcevc"}
)  # fmt: skip
PARADIGMS = frozenset({"classic", "hybrid-neural", "neural-e2e", "ml-assisted"})
CLASSES = frozenset(
    {"delivery", "mezzanine", "lossless", "low-latency", "texture", "research",
     "distortion-source", "decode-only", "commercial-listed"}
)  # fmt: skip
STATUSES = frozenset({"implemented", "planned", "listed", "decision"})
LICENCES = frozenset(
    {"permissive", "copyleft-weak", "copyleft-strong", "patent-licensed-binary",
     "proprietary-sdk", "research-noncommercial", "unknown"}
)  # fmt: skip
MAINTENANCE = frozenset({"active", "slow", "archived", "dead", "unknown"})
INTERFACES = frozenset({"ffmpeg", "cli", "sdk", "decode-only"})
HARDWARE = frozenset({"", "nvenc", "qsv", "amf", "vaapi", "vulkan", "videotoolbox", "mediacodec"})
USE_CASES = frozenset(
    {"vod-ladder", "archival-grain", "broadcast-hrd", "low-latency", "interlaced",
     "intra-camera", "lossless-archive", "texture-playback", "calibration"}
)  # fmt: skip
SURFACES = frozenset({"go", "python"})
KNOBS = frozenset({"crf", "cq", "global_quality", "qp", "q:v", "profile:v"})
PRESET_STYLES = frozenset(
    {"verbatim", "nvenc", "svtav1", "libaom-cpu-used", "amf-quality", "vvenc",
     "vpx-cpu-used", "realtime"}
)  # fmt: skip
VARIANT_KINDS = frozenset({"runtime-fork", "tuning-profile"})


@dataclass(frozen=True)
class Adapter:
    knob: str
    quality_range: tuple[int, int]
    absolute_range: tuple[int, int]
    quality_default: int
    invert_quality: bool
    presets: tuple[str, ...]
    preset_style: str
    version: str
    probe_preset: str
    probe_quality: int
    supports_qpfile: bool
    supports_encoder_stats: bool
    supports_two_pass: bool
    two_pass_abr_at_pass1_bitrate: bool


@dataclass(frozen=True)
class Encoder:
    id: str
    family: str
    paradigm: str
    klass: str
    status: str
    interface: str
    ffmpeg_encoder: str
    hardware: str
    licence_class: str
    maintenance: str
    upstream: str
    checked: str
    use_cases: tuple[str, ...]
    surfaces: tuple[str, ...]
    adapter: Adapter | None
    notes: str


@dataclass(frozen=True)
class Variant:
    adapter: str
    variant: str
    kind: str
    status: str
    licence_class: str
    upstream: str
    checked: str
    notes: str


@dataclass(frozen=True)
class Registry:
    encoders: tuple[Encoder, ...]
    variants: tuple[Variant, ...]


def _choice(entry: Entry, key: str, allowed: frozenset[str], where: str) -> str:
    value = need(entry, key, where)
    if value not in allowed:
        raise DefinitionError(f"{where}: `{key}` = {value!r} is not one of {sorted(allowed)}")
    return str(value)


def _choices(entry: Entry, key: str, allowed: frozenset[str], where: str) -> tuple[str, ...]:
    values = entry.get(key, [])
    if not isinstance(values, list) or len(values) > len(allowed):
        raise DefinitionError(f"{where}: `{key}` must be a list of {sorted(allowed)}")
    for value in values:
        if value not in allowed:
            raise DefinitionError(f"{where}: `{key}` has {value!r}, not one of {sorted(allowed)}")
    if len(set(values)) != len(values):
        raise DefinitionError(f"{where}: `{key}` repeats a value")
    return tuple(values)


def _range(entry: Entry, key: str, where: str) -> tuple[int, int]:
    value = need(entry, key, where)
    if not (isinstance(value, list) and len(value) == PAIR and all(type(v) is int for v in value)):
        raise DefinitionError(f"{where}: `{key}` must be [low, high] integers")
    low, high = value
    if low > high:
        raise DefinitionError(f"{where}: `{key}` low {low} is above high {high}")
    return low, high


def _flag(table: Entry, key: str, where: str, required: bool = False) -> bool:
    value = need(table, key, where) if required else table.get(key, False)
    if type(value) is not bool:
        raise DefinitionError(f"{where}: `{key}` must be true or false")
    return value


def _adapter(entry: Entry, where: str) -> Adapter:
    table = need(entry, "adapter", where)
    here = f"{where}.adapter"
    window = _range(table, "quality_range", here)
    absolute = _range(table, "absolute_range", here)
    default = need(table, "quality_default", here)
    if not absolute[0] <= window[0] <= window[1] <= absolute[1]:
        raise DefinitionError(
            f"{here}: quality_range {window} lies outside absolute_range {absolute}"
        )
    if type(default) is not int or not window[0] <= default <= window[1]:
        raise DefinitionError(f"{here}: quality_default {default!r} lies outside {window}")
    presets = need(table, "presets", here)
    if not isinstance(presets, list) or not presets or len(presets) > MAX_PRESETS:
        raise DefinitionError(f"{here}: `presets` must list 1..{MAX_PRESETS} names")
    probe_preset = str(need(table, "probe_preset", here))
    if probe_preset not in presets:
        raise DefinitionError(f"{here}: probe_preset {probe_preset!r} is not in `presets`")
    probe_quality = need(table, "probe_quality", here)
    if type(probe_quality) is not int or not absolute[0] <= probe_quality <= absolute[1]:
        raise DefinitionError(f"{here}: probe_quality {probe_quality!r} lies outside {absolute}")
    return Adapter(
        knob=_choice(table, "knob", KNOBS, here),
        quality_range=window,
        absolute_range=absolute,
        quality_default=default,
        invert_quality=_flag(table, "invert_quality", here, required=True),
        presets=tuple(str(p) for p in presets),
        preset_style=_choice(table, "preset_style", PRESET_STYLES, here),
        version=str(need(table, "version", here)),
        probe_preset=probe_preset,
        probe_quality=probe_quality,
        supports_qpfile=_flag(table, "supports_qpfile", here),
        supports_encoder_stats=_flag(table, "supports_encoder_stats", here),
        supports_two_pass=_flag(table, "supports_two_pass", here),
        two_pass_abr_at_pass1_bitrate=_flag(table, "two_pass_abr_at_pass1_bitrate", here),
    )


def _checked(entry: Entry, where: str) -> str:
    value = str(need(entry, "checked", where))
    if not DATE.fullmatch(value):
        raise DefinitionError(f"{where}: `checked` must be YYYY-MM-DD, got {value!r}")
    return value


def _token(entry: Entry, key: str, where: str) -> str:
    value = str(need(entry, key, where))
    if not TOKEN.fullmatch(value):
        raise DefinitionError(f"{where}: `{key}` {value!r} is not a lower-case token")
    return value


def _encoder(entry: Entry) -> Encoder:
    where = f"encoders[{entry.get('id', '?')}]"
    surfaces = _choices(entry, "surfaces", SURFACES, where)
    status = _choice(entry, "status", STATUSES, where)
    if bool(surfaces) != (status == "implemented"):
        raise DefinitionError(f"{where}: `surfaces` is set exactly when status is implemented")
    if bool(surfaces) != ("adapter" in entry):
        raise DefinitionError(f"{where}: an `adapter` table is set exactly when `surfaces` is")
    interface = _choice(entry, "interface", INTERFACES, where)
    ffmpeg = str(entry.get("ffmpeg_encoder", ""))
    if (interface == "ffmpeg") != bool(ffmpeg):
        raise DefinitionError(f"{where}: `ffmpeg_encoder` is set exactly when interface is ffmpeg")
    return Encoder(
        id=_token(entry, "id", where),
        family=_choice(entry, "family", FAMILIES, where),
        paradigm=_choice(entry, "paradigm", PARADIGMS, where),
        klass=_choice(entry, "class", CLASSES, where),
        status=status,
        interface=interface,
        ffmpeg_encoder=ffmpeg,
        hardware=_choice({"hardware": entry.get("hardware", "")}, "hardware", HARDWARE, where),
        licence_class=_choice(entry, "licence_class", LICENCES, where),
        maintenance=_choice(entry, "maintenance", MAINTENANCE, where),
        upstream=str(need(entry, "upstream", where)),
        checked=_checked(entry, where),
        use_cases=_choices(entry, "use_cases", USE_CASES, where),
        surfaces=surfaces,
        adapter=_adapter(entry, where) if surfaces else None,
        notes=str(entry.get("notes", "")),
    )


def _variant(entry: Entry, known: frozenset[str]) -> Variant:
    where = f"variants[{entry.get('adapter', '?')}@{entry.get('variant', '?')}]"
    adapter = _token(entry, "adapter", where)
    if adapter not in known:
        raise DefinitionError(f"{where}: adapter {adapter!r} has no [[encoders]] row")
    return Variant(
        adapter=adapter,
        variant=_token(entry, "variant", where),
        kind=_choice(entry, "kind", VARIANT_KINDS, where),
        status=_choice(entry, "status", STATUSES, where),
        licence_class=_choice(entry, "licence_class", LICENCES, where),
        upstream=str(need(entry, "upstream", where)),
        checked=_checked(entry, where),
        notes=str(entry.get("notes", "")),
    )


def _table(data: dict[str, Any], key: str) -> list[Entry]:
    rows = data.get(key, [])
    if not isinstance(rows, list) or len(rows) > MAX_ROWS:
        raise DefinitionError(f"{DEFINITION}: `{key}` must be a list of at most {MAX_ROWS} tables")
    return rows


def parse(data: dict[str, Any]) -> Registry:
    """The registry of a parsed definition; every rule above is checked."""
    if data.get("format_version") != FORMAT_VERSION:
        raise DefinitionError(f"{DEFINITION}: format_version must be {FORMAT_VERSION}")
    encoders = tuple(_encoder(e) for e in _table(data, "encoders"))
    ids = [e.id for e in encoders]
    if len(set(ids)) != len(ids):
        raise DefinitionError(f"{DEFINITION}: an encoder id appears twice")
    variants = tuple(_variant(v, frozenset(ids)) for v in _table(data, "variants"))
    tokens = [f"{v.adapter}@{v.variant}" for v in variants]
    if len(set(tokens)) != len(tokens):
        raise DefinitionError(f"{DEFINITION}: a variant token appears twice")
    return Registry(encoders=encoders, variants=variants)


def load(path: Path) -> Registry:
    with path.open("rb") as handle:
        return parse(tomllib.load(handle))
