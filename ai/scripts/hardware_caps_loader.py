#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Hardware-capability fingerprint loader (ADR-0335).

Reads ``ai/data/hardware_caps.csv`` and emits per-row capability
feature vectors that the corpus-ingest pipeline merges into encode
rows. The loader is **prior-only**: it returns metadata about what a
GPU generation *can do* (codecs, resolution caps, encoder-block
count, tensor / NPU presence) and never benchmark numbers
(throughput, watts, latency, quality scores).

The contributor-pack research digest
(``docs/research/0088-hardware-capability-priors-2026-05-08.md``)
flagged benchmark fields as a NO-GO: shipping vendor-published
throughput numbers leaks biased priors into the FR-regressor. Pure
capability metadata does not have that pathology — a "this GPU can
emit AV1" bit is a structural fact, not a measurement.

Usage from a corpus-ingest script::

    from ai.scripts.hardware_caps_loader import (
        HardwareCapsTable,
        cap_vector_for,
    )

    caps = HardwareCapsTable.load_default()
    row = cap_vector_for(
        caps,
        encoder="av1_nvenc",
        encoder_arch_hint="blackwell",
    )
    # row -> dict of feature columns to merge into the corpus row

The table is small (~10 rows) so it loads eagerly into a list of
dataclasses. CSV header lines starting with ``#`` are stripped.
"""

from __future__ import annotations

import csv
import io
import re
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Iterable

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
_REPO_ROOT = _SCRIPT_PATHS.repo_root
_DEFAULT_CSV = _REPO_ROOT / "ai" / "data" / "hardware_caps.csv"

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402

REQUIRED_COLUMNS: tuple[str, ...] = (
    "arch_name",
    "vendor",
    "gen_year",
    "codecs_supported",
    "max_resolution_per_codec",
    "encoding_blocks",
    "tensor_cores",
    "npu_present",
    "driver_min_version",
    "source_url",
    "verified_date",
)

# Map ffmpeg / vmaf-tune encoder names to the codec token used in the
# CSV. Keep the table conservative — only encoders the fork actually
# routes through the corpus today are listed. Unknown encoders raise
# rather than silently fall back to a wrong arch.
ENCODER_TO_CODEC: dict[str, str] = {
    # NVIDIA NVENC
    "h264_nvenc": "h264",
    "hevc_nvenc": "hevc",
    "av1_nvenc": "av1",
    # AMD AMF
    "h264_amf": "h264",
    "hevc_amf": "hevc",
    "av1_amf": "av1",
    # Intel Quick Sync Video / oneVPL
    "h264_qsv": "h264",
    "hevc_qsv": "hevc",
    "av1_qsv": "av1",
}


class HardwareCapsError(ValueError):
    """Raised on schema violations in ``hardware_caps.csv``."""


@dataclass(frozen=True)
class HardwareCapRow:
    """One row of the hardware capability fingerprint table."""

    arch_name: str
    vendor: str
    gen_year: int
    codecs_supported: tuple[str, ...]
    max_resolution_per_codec: dict[str, tuple[int, int]]
    encoding_blocks: int
    tensor_cores: bool
    npu_present: bool
    driver_min_version: str
    source_url: str
    verified_date: str

    def max_resolution_for(self, codec: str) -> tuple[int, int] | None:
        """Return ``(width, height)`` for the requested codec or ``None``."""
        return self.max_resolution_per_codec.get(codec)


@dataclass
class HardwareCapsTable:
    """In-memory view of ``hardware_caps.csv``."""

    rows: list[HardwareCapRow] = field(default_factory=list)

    @classmethod
    def load_default(cls) -> "HardwareCapsTable":
        """Load the in-tree default CSV at ``ai/data/hardware_caps.csv``."""
        return cls.load(_DEFAULT_CSV)

    @classmethod
    def load(cls, path: Path) -> "HardwareCapsTable":
        """Load and validate a hardware-capability CSV from disk."""
        text = Path(path).read_text(encoding="utf-8")
        return cls.loads(text, source=str(path))

    @classmethod
    def loads(cls, text: str, *, source: str = "<string>") -> "HardwareCapsTable":
        """Parse a CSV string already loaded into memory."""
        # Strip leading comment block (``# ...`` lines).
        lines = [
            line for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")
        ]
        if not lines:
            raise HardwareCapsError(f"{source}: empty after stripping comments")
        reader = csv.DictReader(io.StringIO("\n".join(lines)))
        if reader.fieldnames is None:
            raise HardwareCapsError(f"{source}: missing CSV header")
        missing = [c for c in REQUIRED_COLUMNS if c not in reader.fieldnames]
        if missing:
            raise HardwareCapsError(f"{source}: missing required columns: {missing}")
        rows: list[HardwareCapRow] = []
        for raw in reader:
            rows.append(_parse_row(raw, source=source))
        if not rows:
            raise HardwareCapsError(f"{source}: no data rows")
        _check_unique_arch(rows, source=source)
        return cls(rows=rows)

    def by_arch(self, arch_name: str) -> HardwareCapRow | None:
        """Return the row whose ``arch_name`` matches ``arch_name``."""
        norm = _normalise_arch(arch_name)
        for row in self.rows:
            if row.arch_name == norm:
                return row
        return None

    def __iter__(self) -> Iterable[HardwareCapRow]:  # type: ignore[override]
        return iter(self.rows)

    def __len__(self) -> int:
        return len(self.rows)


def _parse_row(raw: dict[str, str], *, source: str) -> HardwareCapRow:
    """Convert a raw CSV ``dict`` to a typed :class:`HardwareCapRow`."""
    for col in REQUIRED_COLUMNS:
        value = raw.get(col, "")
        if value is None or str(value).strip() == "":
            raise HardwareCapsError(f"{source}: row {raw!r}: column {col!r} is empty")
    arch_name = _normalise_arch(raw["arch_name"])
    vendor = raw["vendor"].strip().lower()
    if vendor not in {"intel", "amd", "nvidia"}:
        raise HardwareCapsError(
            f"{source}: arch={arch_name!r}: vendor must be intel|amd|nvidia, " f"got {vendor!r}"
        )
    try:
        gen_year = int(raw["gen_year"])
    except ValueError as exc:
        raise HardwareCapsError(f"{source}: arch={arch_name!r}: gen_year must be int") from exc
    if not 2000 <= gen_year <= 2100:
        raise HardwareCapsError(f"{source}: arch={arch_name!r}: gen_year out of plausible range")
    codecs = tuple(c.strip().lower() for c in raw["codecs_supported"].split("|"))
    if not codecs or any(not c for c in codecs):
        raise HardwareCapsError(f"{source}: arch={arch_name!r}: codecs_supported is empty")
    max_res = _parse_max_res(
        raw["max_resolution_per_codec"], codecs, arch_name=arch_name, source=source
    )
    blocks = int(raw["encoding_blocks"])
    if blocks < 1:
        raise HardwareCapsError(
            f"{source}: arch={arch_name!r}: encoding_blocks must be >= 1; "
            "rows with zero encode engines belong outside this table"
        )
    tensor_cores = _parse_bool(raw["tensor_cores"], "tensor_cores", arch_name)
    npu_present = _parse_bool(raw["npu_present"], "npu_present", arch_name)
    driver_min_version = raw["driver_min_version"].strip()
    source_url = raw["source_url"].strip()
    if not source_url.startswith(("http://", "https://")):
        raise HardwareCapsError(f"{source}: arch={arch_name!r}: source_url must be http(s)")
    if "wikipedia.org" in source_url or "wikichip.org" in source_url:
        raise HardwareCapsError(
            f"{source}: arch={arch_name!r}: source_url is a community wiki; "
            "primary vendor docs only"
        )
    verified_date = raw["verified_date"].strip()
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", verified_date):
        raise HardwareCapsError(f"{source}: arch={arch_name!r}: verified_date must be ISO-8601")
    return HardwareCapRow(
        arch_name=arch_name,
        vendor=vendor,
        gen_year=gen_year,
        codecs_supported=codecs,
        max_resolution_per_codec=max_res,
        encoding_blocks=blocks,
        tensor_cores=tensor_cores,
        npu_present=npu_present,
        driver_min_version=driver_min_version,
        source_url=source_url,
        verified_date=verified_date,
    )


def _parse_max_res(
    raw: str,
    codecs: tuple[str, ...],
    *,
    arch_name: str,
    source: str,
) -> dict[str, tuple[int, int]]:
    out: dict[str, tuple[int, int]] = {}
    for entry in raw.split("|"):
        entry = entry.strip()
        if not entry:
            continue
        if "=" not in entry:
            raise HardwareCapsError(
                f"{source}: arch={arch_name!r}: max_res entry {entry!r} " "must be codec=WxH"
            )
        codec, dims = entry.split("=", 1)
        codec = codec.strip().lower()
        match = re.fullmatch(r"(\d+)x(\d+)", dims.strip())
        if not match:
            raise HardwareCapsError(
                f"{source}: arch={arch_name!r}: max_res dims {dims!r} " "must be WxH"
            )
        out[codec] = (int(match.group(1)), int(match.group(2)))
    missing = [c for c in codecs if c not in out]
    if missing:
        raise HardwareCapsError(
            f"{source}: arch={arch_name!r}: max_resolution_per_codec is "
            f"missing entries for {missing}"
        )
    return out


def _parse_bool(value: str, name: str, arch_name: str) -> bool:
    v = value.strip().lower()
    if v in {"1", "true", "yes"}:
        return True
    if v in {"0", "false", "no"}:
        return False
    raise HardwareCapsError(f"arch={arch_name!r}: {name} must be 0|1, got {value!r}")


def _check_unique_arch(rows: list[HardwareCapRow], *, source: str) -> None:
    seen: set[str] = set()
    for row in rows:
        if row.arch_name in seen:
            raise HardwareCapsError(f"{source}: duplicate arch_name {row.arch_name!r}")
        seen.add(row.arch_name)


def _normalise_arch(arch: str) -> str:
    return arch.strip().lower().replace(" ", "-").replace("_", "-")


def cap_vector_for(
    caps: HardwareCapsTable,
    *,
    encoder: str,
    encoder_arch_hint: str | None,
) -> dict[str, object]:
    """Return capability feature columns for one corpus row.

    Parameters
    ----------
    caps:
        Loaded :class:`HardwareCapsTable`.
    encoder:
        ffmpeg / vmaf-tune encoder name (e.g. ``"av1_nvenc"``,
        ``"hevc_qsv"``, ``"av1_amf"``). Maps to a codec token via
        :data:`ENCODER_TO_CODEC`. CPU-only encoders (``libx264``,
        ``libx265``, ``libsvtav1`` …) are out of scope and return an
        all-``None`` fingerprint with ``cap_known=0``.
    encoder_arch_hint:
        Architecture key sourced from the corpus ingest pipeline
        (``"battlemage"``, ``"rdna4"``, ``"blackwell"`` …). May be
        ``None`` if the ingest could not detect the arch — the
        function returns an all-``None`` fingerprint with
        ``cap_known=0``.

    Returns
    -------
    dict
        A flat dict of feature columns suitable for direct merge into
        a corpus row. The shape is stable regardless of whether the
        arch was resolved (missing values are encoded as ``None`` so
        downstream parquet writers flag them as nulls). All keys are
        prefixed with ``hwcap_``.
    """
    keys = (
        "hwcap_known",
        "hwcap_arch_name",
        "hwcap_vendor",
        "hwcap_gen_year",
        "hwcap_codec",
        "hwcap_codec_supported",
        "hwcap_max_width",
        "hwcap_max_height",
        "hwcap_encoding_blocks",
        "hwcap_tensor_cores",
        "hwcap_npu_present",
        "hwcap_driver_min_version",
        "hwcap_source_url",
        "hwcap_verified_date",
    )
    blank: dict[str, object] = {k: None for k in keys}
    blank["hwcap_known"] = 0

    if encoder_arch_hint is None:
        return blank
    if encoder not in ENCODER_TO_CODEC:
        # CPU-only software encoder or one we deliberately do not
        # fingerprint. The all-None vector preserves the corpus
        # column shape.
        return blank
    row = caps.by_arch(encoder_arch_hint)
    if row is None:
        return blank
    codec = ENCODER_TO_CODEC[encoder]
    supported = codec in row.codecs_supported
    max_wh = row.max_resolution_for(codec) if supported else None
    return {
        "hwcap_known": 1,
        "hwcap_arch_name": row.arch_name,
        "hwcap_vendor": row.vendor,
        "hwcap_gen_year": row.gen_year,
        "hwcap_codec": codec,
        "hwcap_codec_supported": int(supported),
        "hwcap_max_width": max_wh[0] if max_wh else None,
        "hwcap_max_height": max_wh[1] if max_wh else None,
        "hwcap_encoding_blocks": row.encoding_blocks,
        "hwcap_tensor_cores": int(row.tensor_cores),
        "hwcap_npu_present": int(row.npu_present),
        "hwcap_driver_min_version": row.driver_min_version,
        "hwcap_source_url": row.source_url,
        "hwcap_verified_date": row.verified_date,
    }


def row_as_dict(row: HardwareCapRow) -> dict[str, object]:
    """Serialise a row back to a JSON-friendly dict (for diagnostics)."""
    out = asdict(row)
    out["codecs_supported"] = list(row.codecs_supported)
    out["max_resolution_per_codec"] = {k: list(v) for k, v in row.max_resolution_per_codec.items()}
    out["tensor_cores"] = bool(row.tensor_cores)
    out["npu_present"] = bool(row.npu_present)
    return out


def _main(argv: list[str] | None = None) -> int:  # pragma: no cover
    """Tiny CLI: print the loaded table as JSON for debugging."""
    import json

    raw_argv = collect_cli_argv(argv)
    parser = make_argument_parser(
        prog="hardware_caps_loader.py",
        description=__doc__,
    )
    parser.add_argument(
        "--csv",
        type=Path,
        default=_DEFAULT_CSV,
        help="path to hardware_caps.csv",
    )
    parser.add_argument(
        "--encoder",
        help="lookup encoder + arch and print the cap vector",
    )
    parser.add_argument(
        "--arch",
        help="architecture hint to pair with --encoder",
    )
    args = parser.parse_args(raw_argv)

    table = HardwareCapsTable.load(args.csv)
    if args.encoder:
        vec = cap_vector_for(table, encoder=args.encoder, encoder_arch_hint=args.arch)
        print(json.dumps(vec, indent=2, sort_keys=True))
        return 0
    print(
        json.dumps(
            {"rows": [row_as_dict(r) for r in table.rows]},
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(_main())
