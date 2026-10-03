# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Which Metal state rows a report measures, and with what result (ADR-1496).

`image/metal-rows.json` names, for every open Metal row of `docs/state.md`,
the measurements that close it on an Apple device:

- `case`: a case of a Metal parity test (`@case` lines, hw_suites.py);
- `metric`: Metal scores of a twin on every fixture of the Metal
  equivalence, compared with the CPU's (`bound` 0 is `==`); the twin must
  have run on Metal there, a CPU fallback does not count;
- `gate`: a cell of the parity gate's Metal run on every fixture (hw_gate.py).

A row is `pass` when every measurement it names passed on this device, `fail`
when one failed, and `not_measured` otherwise (no device, a skipped case, a
check not run). `pass` is evidence for the maintainer, who closes the row.
"""

from __future__ import annotations

import json
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from .hw_equiv import Scores

PASSING = ("pass", "identical", "within_bound", "OK")
FAILING = ("fail", "differing", "absent", "not_on_metal", "FAIL", "ERROR")


def load_row_map(path: Path) -> dict[str, Any] | None:
    """The bundle's row map, or None when this package has none (container image)."""
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def case_evidence(spec: Mapping[str, str], cases: Mapping[str, Mapping[str, str]]) -> dict:
    verdict = cases.get(spec["test"], {}).get(spec["case"], "not_run")
    return {"kind": "case", "test": spec["test"], "case": spec["case"], "result": verdict}


def _series_result(
    cpu: Sequence[float | None] | None, metal: Sequence[float | None] | None, bound: float
) -> tuple[str, float]:
    if cpu is None or metal is None or len(cpu) != len(metal):
        return "absent", 0.0
    pairs = list(zip(cpu, metal, strict=True))
    if all(a == b for a, b in pairs):
        return "identical", 0.0
    if any((a is None) != (b is None) for a, b in pairs):
        return "differing", 0.0
    largest = max(abs(a - b) for a, b in pairs if a is not None and b is not None)
    return ("within_bound" if largest <= bound else "differing"), largest


def metric_evidence(
    spec: Mapping[str, Any],
    cpu_scores: Mapping[str, Scores],
    metal_runs: Mapping[str, Mapping[str, Any]],
) -> list[dict[str, Any]]:
    """One item per fixture and metric; `not_run` when the fixture has no Metal run."""
    items: list[dict[str, Any]] = []
    for fixture in sorted(cpu_scores):
        run = metal_runs.get(fixture)
        for metric in spec["metrics"]:
            item: dict[str, Any] = {"kind": "metric", "fixture": fixture,
                                    "extractor": spec["extractor"], "metric": metric}  # fmt: skip
            if run is None:
                item["result"] = "not_run"
            elif spec["extractor"] not in run["on_metal"]:
                item["result"] = "not_on_metal"
            else:
                result, largest = _series_result(
                    cpu_scores[fixture].get(metric), run["scores"].get(metric),
                    float(spec.get("bound", 0.0)),
                )  # fmt: skip
                item["result"] = result
                item["max_abs_diff"] = f"{largest:.17g}"
            items.append(item)
    return items


def gate_evidence(spec: Mapping[str, str], gate: Mapping[str, Any]) -> list[dict[str, Any]]:
    """One item per fixture the gate ran the feature on; a feature left out of a
    fixture for a recorded reason is no item; `not_run` when the gate ran none."""
    items = []
    for entry in gate.get("fixtures", []):
        if any(item["feature"] == spec["feature"] for item in entry.get("left_out", [])):
            continue
        cells = [c for c in entry.get("cells", []) if c["feature"] == spec["feature"]]
        item = {"kind": "gate", "fixture": entry["fixture"], "feature": spec["feature"]}
        item["result"] = cells[0]["status"] if cells else "not_run"
        if cells:
            item["max_abs_diff"] = cells[0]["max_abs_diff"]
        items.append(item)
    return items or [{"kind": "gate", "feature": spec["feature"], "result": "not_run"}]


def verdict_of(evidence: Sequence[Mapping[str, Any]]) -> str:
    """`fail` on any failed measurement, `pass` when all passed, else `not_measured`."""
    results = [item["result"] for item in evidence]
    if any(result in FAILING for result in results):
        return "fail"
    if results and all(result in PASSING for result in results):
        return "pass"
    return "not_measured"


def evaluate_row(
    row: Mapping[str, Any],
    cases: Mapping[str, Mapping[str, str]],
    cpu_scores: Mapping[str, Scores],
    metal_runs: Mapping[str, Mapping[str, Any]],
    gate: Mapping[str, Any],
) -> dict[str, Any]:
    evidence = [case_evidence(spec, cases) for spec in row.get("cases", [])]
    for spec in row.get("metrics", []):
        evidence += metric_evidence(spec, cpu_scores, metal_runs)
    for spec in row.get("gate", []):
        evidence += gate_evidence(spec, gate)
    result = {"id": row["id"], "verdict": verdict_of(evidence), "evidence": evidence}
    if row.get("device_free"):
        result["device_free"] = row["device_free"]
    return result


def evaluate_rows(
    row_map: Mapping[str, Any] | None,
    cases: Mapping[str, Mapping[str, str]],
    cpu_scores: Mapping[str, Scores],
    metal_runs: Mapping[str, Mapping[str, Any]],
    gate: Mapping[str, Any],
) -> dict[str, Any]:
    """The `metal_rows` section of the report."""
    if row_map is None:
        return {"status": "not_applicable", "rows": []}
    rows = [evaluate_row(r, cases, cpu_scores, metal_runs, gate) for r in row_map["rows"]]
    counts = {
        v: sum(1 for r in rows if r["verdict"] == v) for v in ("pass", "fail", "not_measured")
    }
    status = (
        "fail" if counts["fail"] else ("pass" if counts["pass"] == len(rows) else "not_measured")
    )
    return {"status": status, "counts": counts, "rows": rows}
