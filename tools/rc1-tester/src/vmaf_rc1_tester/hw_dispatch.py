# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Executed dispatch level and library identity of a tester report (schema 4).

The library names what it ran in the `provenance` record of its JSON report
(ADR-2073): `simd` is the highest ISA level the CPU extractors dispatch to under
the run's CPU mask. The tester reads it there and compares it with the level its
own host facts predict; it never derives the executed level itself. What the
library does not state is recorded as `not_reported`, not guessed.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

# Highest first; the names are those of `simd_level_of()` in
# core/src/vmafx/provenance_build.c.
LEVEL_ORDER = ("avx512icl", "avx512", "avx2", "sse4.1", "ssse3", "sse2", "sve2", "neon")
SCALAR = "scalar"
# The pairs of shared libraries a vmaf binary can link (ADR-1852 decision D3), as
# their NUL-delimited names in the binary's dynamic string table.
LIBRARY_PAIRS = ("libvmafx.so.1", "libvmaf.so.3")
CONFORMANCE_MODES = ("reference-exact", "netflix-compat", "not_reported")
PROVENANCE_KEYS = ("simd", "digest", "version", "commit", "build_id")
STATUSES = ("match", "mismatch", "not_reported", "not_run")
MAX_BINARY_BYTES = 512 * 1024 * 1024
CHUNK = 1 << 20


def provenance_facts(document: Mapping[str, Any]) -> dict[str, Any] | None:
    """The library's own statements from a vmaf JSON report, or None without a record."""
    record = document.get("provenance")
    if not isinstance(record, Mapping) or not isinstance(record.get("simd"), str):
        return None
    facts: dict[str, Any] = {
        k: record[k] for k in PROVENANCE_KEYS if isinstance(record.get(k), str)
    }
    abi = [record.get(f"abi_{part}") for part in ("major", "minor", "patch")]
    if all(isinstance(n, int) for n in abi):
        facts["abi"] = ".".join(str(n) for n in abi)
    mode = record.get("conformance_mode")
    if isinstance(mode, str):
        facts["conformance_mode"] = mode
    return facts


def reported_level(flags: Sequence[str]) -> str:
    """The level the host flags select: the first of LEVEL_ORDER present, else scalar."""
    return next((level for level in LEVEL_ORDER if level in flags), SCALAR)


def library_pair(vmaf: Path) -> dict[str, str]:
    """Which library pair the vmaf binary links, from its dynamic string table.

    `unknown` when neither name is there (a static or non-ELF binary); the source
    field says how the answer was found, so the report never claims more.
    """
    found: set[str] = set()
    needles = {name: b"\0" + name.encode("ascii") + b"\0" for name in LIBRARY_PAIRS}
    tail = b""
    try:
        with vmaf.open("rb") as handle:
            read = 0
            while read < MAX_BINARY_BYTES:
                chunk = handle.read(CHUNK)
                if not chunk:
                    break
                read += len(chunk)
                window = tail + chunk
                found |= {name for name, needle in needles.items() if needle in window}
                tail = window[-32:]
    except OSError:
        return {"pair": "unknown", "pair_source": "the vmaf binary could not be read"}
    source = "NEEDED names in the vmaf binary's dynamic string table"
    if len(found) == 1:
        return {"pair": next(iter(found)), "pair_source": source}
    return {"pair": "unknown", "pair_source": source + " (none or both found)"}


def _executed(cells: Sequence[Mapping[str, Any]]) -> list[Mapping[str, Any]]:
    return [cell["executed"] for cell in cells if isinstance(cell.get("executed"), Mapping)]


def fixture_status(executed: Mapping[str, Any], reported: str) -> str:
    """`match` when the default run reports `reported` and the masked run `scalar`."""
    if executed.get("default_simd") is None or executed.get("scalar_simd") is None:
        return "not_reported"
    ok = executed["default_simd"] == reported and executed["scalar_simd"] == SCALAR
    return "match" if ok else "mismatch"


def section_status(fixtures: Sequence[Mapping[str, Any]], expected: int) -> str:
    """Status of the section from its fixtures; every fixture of the run must report."""
    if len(fixtures) != expected or not fixtures:
        return "not_reported"
    states = {fixture["status"] for fixture in fixtures}
    for state in ("mismatch", "not_reported"):
        if state in states:
            return state
    return "match"


def executed_dispatch(dispatch: Mapping[str, Any], host_flags: Sequence[str]) -> dict[str, Any]:
    """The `executed_dispatch` section from the dispatch run's cells and the host flags."""
    cells = dispatch.get("fixtures", [])
    if dispatch.get("status") in (None, "not_run"):
        return {"status": "not_run"}
    reported = reported_level(host_flags)
    fixtures = []
    for cell in cells:
        executed = cell.get("executed") or {}
        fixtures.append({
            "fixture": cell["fixture"],
            "default_simd": executed.get("default_simd"),
            "scalar_simd": executed.get("scalar_simd"),
            "status": fixture_status(executed, reported),
        })  # fmt: skip
    return {
        "status": section_status(fixtures, len(cells)),
        "reported": reported,
        "reported_source": "host dispatch_flags (mirror of the library's CPU rules)",
        "fixtures": fixtures,
    }


def library_block(vmaf: Path, dispatch: Mapping[str, Any]) -> dict[str, Any]:
    """Library identity: the pair the binary links and what the library says of itself."""
    block: dict[str, Any] = dict(library_pair(vmaf))
    executed = _executed(dispatch.get("fixtures", []))
    first = executed[0] if executed else {}
    for key in ("version", "commit", "build_id", "abi", "provenance_digest"):
        block[key] = first.get(key)
    block["conformance_mode"] = first.get("conformance_mode") or "not_reported"
    return block
