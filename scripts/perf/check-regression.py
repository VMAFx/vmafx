#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Wall-clock perf regression gate over the ADR-0752 multi-resolution baseline.

Reads the committed baseline at testdata/perf_multi_resolution.json and a fresh
run JSON produced by scripts/perf/bench-multi-resolution.sh, joins them by
(resolution, backend, metric), and exits non-zero when any cell's median_ms
exceeds the baseline by more than --tolerance-pct (default 5%).

Cells with status != "ok" in either side are reported and skipped, not failed,
so missing GPU lanes do not block CPU-only CI runs.

Reference: ADR-0907 — Wall-clock perf regression gate.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

DEFAULT_BASELINE = Path("testdata/perf_multi_resolution.json")
DEFAULT_TOLERANCE_PCT = 5.0

# Exit code used for regression findings (overridden to 0 in --advisory mode).
_EXIT_REGRESSION = 1


def _cell_key(cell: dict[str, Any]) -> tuple[str, str, str]:
    """Canonical join key: (resolution, backend, metric)."""
    return (str(cell["resolution"]), str(cell["backend"]), str(cell["metric"]))


def _index_runs(payload: dict[str, Any]) -> dict[tuple[str, str, str], dict[str, Any]]:
    """Build a (resolution, backend, metric) -> cell index from a baseline/run JSON."""
    runs = payload.get("runs", [])
    if not isinstance(runs, list):
        raise ValueError("malformed perf JSON: 'runs' is not a list")
    return {_cell_key(cell): cell for cell in runs}


def _filter_backends(
    cells: dict[tuple[str, str, str], dict[str, Any]],
    *,
    backends: Sequence[str] | None,
) -> dict[tuple[str, str, str], dict[str, Any]]:
    """Optionally restrict to one or more backends (CPU-only gate by default in CI)."""
    if not backends:
        return cells
    wanted = set(backends)
    return {k: v for k, v in cells.items() if k[1] in wanted}


def compare_runs(
    baseline: dict[str, Any],
    current: dict[str, Any],
    *,
    tolerance_pct: float,
    backends: Sequence[str] | None,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]]]:
    """Compare two perf-bench payloads.

    Returns (regressions, improvements, skipped). regressions are cells whose
    current median_ms exceeds baseline by > tolerance_pct.
    """
    base = _filter_backends(_index_runs(baseline), backends=backends)
    cur = _filter_backends(_index_runs(current), backends=backends)

    regressions: list[dict[str, Any]] = []
    improvements: list[dict[str, Any]] = []
    skipped: list[dict[str, Any]] = []

    for key, base_cell in sorted(base.items()):
        cur_cell = cur.get(key)
        if cur_cell is None:
            skipped.append(
                {
                    "key": key,
                    "reason": "missing in current run",
                    "baseline_ms": base_cell.get("median_ms"),
                }
            )
            continue
        if base_cell.get("status") != "ok" or cur_cell.get("status") != "ok":
            skipped.append(
                {
                    "key": key,
                    "reason": f"status baseline={base_cell.get('status')} current={cur_cell.get('status')}",
                    "baseline_ms": base_cell.get("median_ms"),
                    "current_ms": cur_cell.get("median_ms"),
                }
            )
            continue

        base_ms = float(base_cell["median_ms"])
        cur_ms = float(cur_cell["median_ms"])
        if base_ms <= 0:
            skipped.append(
                {"key": key, "reason": "baseline median_ms <= 0", "baseline_ms": base_ms}
            )
            continue

        delta_pct = (cur_ms - base_ms) / base_ms * 100.0
        record = {
            "key": key,
            "baseline_ms": base_ms,
            "current_ms": cur_ms,
            "delta_pct": delta_pct,
        }
        if delta_pct > tolerance_pct:
            regressions.append(record)
        elif delta_pct < -tolerance_pct:
            improvements.append(record)

    return regressions, improvements, skipped


def _format_row(record: dict[str, Any]) -> str:
    res, backend, metric = record["key"]
    return (
        f"  {res:>4}p {backend:>4} {metric:>8} : "
        f"{record['baseline_ms']:>7.1f} ms -> {record['current_ms']:>7.1f} ms "
        f"({record['delta_pct']:+6.2f}%)"
    )


def _baseline_has_ok_cells(
    payload: dict[str, Any],
    *,
    backends: Sequence[str] | None,
) -> bool:
    """Return True when the baseline contains at least one ok cell for the given backends."""
    runs = payload.get("runs", [])
    if not isinstance(runs, list):
        return False
    for cell in runs:
        if cell.get("status") != "ok":
            continue
        if backends and cell.get("backend") not in set(backends):
            continue
        return True
    return False


def render_report(
    regressions: list[dict[str, Any]],
    improvements: list[dict[str, Any]],
    skipped: list[dict[str, Any]],
    *,
    tolerance_pct: float,
    advisory: bool = False,
) -> str:
    """Build a human-readable report (stable layout for CI logs)."""
    mode_tag = " [ADVISORY — exit 0 regardless]" if advisory else ""
    lines = [f"== Perf regression gate (tolerance: +/- {tolerance_pct:.1f}%){mode_tag} =="]
    if regressions:
        lines.append("")
        lines.append(f"REGRESSIONS ({len(regressions)}):")
        for record in regressions:
            lines.append(_format_row(record))
    if improvements:
        lines.append("")
        lines.append(f"Improvements (informational, {len(improvements)}):")
        for record in improvements:
            lines.append(_format_row(record))
    if skipped:
        lines.append("")
        lines.append(f"Skipped ({len(skipped)}):")
        for record in skipped:
            res, backend, metric = record["key"]
            lines.append(f"  {res:>4}p {backend:>4} {metric:>8} : {record['reason']}")
    if not regressions and not improvements and not skipped:
        lines.append("All cells within tolerance.")
    return "\n".join(lines)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--baseline",
        type=Path,
        default=DEFAULT_BASELINE,
        help=f"path to committed baseline JSON (default: {DEFAULT_BASELINE})",
    )
    parser.add_argument(
        "--current",
        type=Path,
        required=True,
        help="path to current run JSON produced by bench-multi-resolution.sh",
    )
    parser.add_argument(
        "--tolerance-pct",
        type=float,
        default=DEFAULT_TOLERANCE_PCT,
        help=f"max allowed wall-clock regression in percent (default: {DEFAULT_TOLERANCE_PCT})",
    )
    parser.add_argument(
        "--backend",
        action="append",
        default=None,
        help="restrict comparison to one or more backends; repeatable (default: all)",
    )
    parser.add_argument(
        "--advisory",
        action="store_true",
        default=False,
        help=(
            "print the full regression report but always exit 0 (ADR-1005). "
            "Use until a CI-runner-calibrated baseline is committed."
        ),
    )
    parser.add_argument(
        "--skip-if-no-baseline",
        action="store_true",
        default=False,
        dest="skip_if_no_baseline",
        help=(
            "exit 0 with an informational message when the baseline file has no "
            "ok cells for the requested backend(s). "
            "Prevents false positives on the first run cycle after a new baseline seed is committed."
        ),
    )
    args = parser.parse_args(argv)
    if args.tolerance_pct <= 0:
        parser.error("--tolerance-pct must be > 0")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    """CLI entry point."""
    args = parse_args(argv)
    try:
        baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
        current = json.loads(args.current.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        sys.stderr.write(f"perf-regression-gate: failed to read input JSON: {exc}\n")
        return 2

    if args.skip_if_no_baseline and not _baseline_has_ok_cells(baseline, backends=args.backend):
        print(
            "perf-regression-gate: baseline has no ok cells for the requested "
            "backend(s); skipping comparison (--skip-if-no-baseline). "
            "Commit a calibrated baseline to enable the gate."
        )
        return 0

    regressions, improvements, skipped = compare_runs(
        baseline,
        current,
        tolerance_pct=args.tolerance_pct,
        backends=args.backend,
    )
    print(
        render_report(
            regressions,
            improvements,
            skipped,
            tolerance_pct=args.tolerance_pct,
            advisory=args.advisory,
        )
    )
    if args.advisory and regressions:
        print(
            f"\nperf-regression-gate: {len(regressions)} regression(s) found "
            "but --advisory mode is active; exiting 0. "
            "See ADR-1005 for the promotion plan."
        )
    return 0 if (args.advisory or not regressions) else _EXIT_REGRESSION


if __name__ == "__main__":
    raise SystemExit(main())
