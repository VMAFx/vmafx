# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Score-level checks of the tester image: dispatch and reference equivalence.

Every CPU feature extractor runs on each fixture at `--precision max` (`%.17g`),
once with the default dispatch and once with every SIMD flag masked off
(`--cpumask`, scalar C). Scores are compared with `==`, never a tolerance.
"""

from __future__ import annotations

import json
import re
import signal
import tempfile
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import Any

from .hw_dispatch import provenance_facts
from .safe_process import run_bounded

# Every extractor registered on the CPU backend that runs on a plain YUV pair.
EXTRACTORS = (
    "float_psnr", "float_adm", "float_vif", "float_motion", "float_moment",
    "speed_chroma", "speed_temporal", "float_ms_ssim", "float_ssim", "ssim",
    "ssimulacra2", "y_funque_plus", "niqe", "brisque", "ciede", "delta_e_itp",
    "pu21", "psnr", "psnr_hvs", "adm", "motion", "motion_v2", "vif", "cambi",
)  # fmt: skip
SCALAR_CPUMASK = 0xFFFFFFFF
MAX_REPORTED_METRICS = 40
MAX_FRAMES = 100_000
# A metric the extractor could not finite-evaluate is JSON null; null == null.
Scores = dict[str, list[float | None]]
Runner = Callable[..., Any]
# The stderr lines of a failed vmaf run the error keeps besides its last line: the
# CLI's `problem ...` / `error: ...` messages and libvmaf's ERROR and WARNING log
# lines. The last line alone is often a warning printed at close, after the
# message that names the failure.
DIAGNOSTIC_LINE = re.compile(
    r"^(?:problem\b|error:|vmaf: (?:error|warning):)|\blibvmaf (?:ERROR|WARNING)\b"
)
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")
MAX_DIAGNOSTIC_LINES = 20
MAX_DIAGNOSTIC_BYTES = 4096
MAX_DIAGNOSTIC_LINE = 300


class FixtureRunError(RuntimeError):
    """The vmaf binary failed or produced unusable output for a fixture."""

    def __init__(self, message: str, returncode: int | None = None) -> None:
        super().__init__(message)
        self.returncode = returncode


def signal_name(code: int) -> str | None:
    """`SIGSEGV` for a process the signal killed (a negative return code), else None."""
    if code >= 0:
        return None
    try:
        return signal.Signals(-code).name
    except ValueError:
        return f"signal {-code}"


def diagnostic_lines(stderr: str) -> list[str]:
    """Every distinct diagnostic line of `stderr` in order, at most
    MAX_DIAGNOSTIC_LINES lines and MAX_DIAGNOSTIC_BYTES bytes, then a count of
    the ones left out."""
    kept: list[str] = []
    seen: set[str] = set()
    size = 0
    left_out = 0
    for raw in stderr.splitlines():
        line = ANSI_ESCAPE.sub("", raw).strip()[:MAX_DIAGNOSTIC_LINE]
        if not DIAGNOSTIC_LINE.search(line) or line in seen:
            continue
        seen.add(line)
        length = len(line.encode("utf-8", "replace")) + 1
        if len(kept) == MAX_DIAGNOSTIC_LINES or size + length > MAX_DIAGNOSTIC_BYTES:
            left_out += 1
            continue
        kept.append(line)
        size += length
    if left_out:
        kept.append(f"({left_out} more lines not kept)")
    return kept


def failure_message(returncode: int, stderr: str) -> str:
    """`vmaf exited N[ (SIGNAL)]: <last stderr line>`, then the diagnostic lines."""
    tail = stderr.strip().splitlines()[-1:] or ["no output"]
    name = signal_name(returncode)
    head = f"vmaf exited {returncode}{f' ({name})' if name else ''}: {tail[0]}"
    lines = diagnostic_lines(stderr)
    if not lines or lines == [ANSI_ESCAPE.sub("", tail[0]).strip()]:
        return head
    return head + "\nstderr problem, error and warning lines:\n" + "\n".join(lines)


def build_argv(
    vmaf: str,
    fixture: Mapping[str, Any],
    out_json: str,
    cpumask: int | None,
    backend: str = "cpu",
) -> list[str]:
    """Command line for one fixture; `cpumask` None selects the default dispatch."""
    argv = [
        vmaf, "--reference", str(fixture["ref"]), "--distorted", str(fixture["dis"]),
        "--width", str(fixture["width"]), "--height", str(fixture["height"]),
        "--pixel_format", str(fixture["pixel_format"]), "--bitdepth", str(fixture["bitdepth"]),
        "--json", "--output", out_json, "--precision", "max", "--threads", "1",
        "--backend", backend, "--quiet",
    ]  # fmt: skip
    for name in EXTRACTORS:
        argv += ["--feature", name]
    if cpumask is not None:
        argv += ["--cpumask", str(cpumask)]
    return argv


def parse_scores(text: str) -> Scores:
    """Per-metric frame series from a vmaf JSON report."""
    try:
        frames = json.loads(text)["frames"]
        scores: Scores = {}
        for index, frame in enumerate(frames[:MAX_FRAMES]):
            if int(frame["frameNum"]) != index:
                raise FixtureRunError("frame numbers are not consecutive")
            for name, value in frame["metrics"].items():
                scores.setdefault(name, []).append(None if value is None else float(value))
    except (KeyError, TypeError, ValueError) as error:
        raise FixtureRunError(f"unreadable vmaf JSON: {error}") from error
    if not scores:
        raise FixtureRunError("vmaf JSON holds no frames")
    return scores


def run_fixture_meta(
    vmaf: str,
    fixture: Mapping[str, Any],
    cpumask: int | None,
    *,
    timeout_seconds: float,
    runner: Runner = run_bounded,
    backend: str = "cpu",
    environment: Mapping[str, str] | None = None,
) -> tuple[Scores, dict[str, Any]]:
    """Run vmaf on one fixture; returns its scores and the JSON's backend facts
    (and, under `log`, the run's stderr lines that name a device)."""
    with tempfile.TemporaryDirectory(prefix="vmaf-tester-") as work:
        out_json = str(Path(work) / "out.json")
        kwargs: dict[str, Any] = {"timeout_seconds": timeout_seconds,
                                  "max_output_bytes": 1_048_576}  # fmt: skip
        if environment is not None:
            kwargs["environment"] = dict(environment)
        result = runner(build_argv(vmaf, fixture, out_json, cpumask, backend), **kwargs)
        if result.returncode != 0:
            message = failure_message(result.returncode, result.stderr or "")
            raise FixtureRunError(message, result.returncode)
        try:
            text = Path(out_json).read_text(encoding="utf-8")
        except OSError as error:
            raise FixtureRunError(f"no output file: {error}") from error
    document = json.loads(text)
    meta = {
        "backend_used": document.get("backend_used"),
        "feature_backends": document.get("feature_backends", []),
        "device_lines": [line for line in (result.stderr or "").splitlines()
                         if "using device" in line][:4],
        "provenance": provenance_facts(document),
    }  # fmt: skip
    return parse_scores(text), meta


def run_fixture(
    vmaf: str,
    fixture: Mapping[str, Any],
    cpumask: int | None,
    *,
    timeout_seconds: float,
    runner: Runner = run_bounded,
) -> Scores:
    """Run vmaf on one fixture and return its scores."""
    return run_fixture_meta(vmaf, fixture, cpumask, timeout_seconds=timeout_seconds, runner=runner)[
        0
    ]


def _fmt(value: float | None) -> str:
    return "null" if value is None else f"{value:.17g}"


def _max_abs_diff(a: Sequence[float | None], b: Sequence[float | None]) -> float:
    """Largest |a - b| over the frames where both are numbers."""
    pairs = [(x, y) for x, y in zip(a, b, strict=True) if x is not None and y is not None]
    return max((abs(x - y) for x, y in pairs), default=0.0)


def _compare_series(
    name: str, a: Sequence[float | None] | None, b: Sequence[float | None] | None
) -> tuple[int, dict[str, Any] | None]:
    """(number of values compared, detail of the difference or None)."""
    if a is None or b is None or len(a) != len(b):
        count = max(len(a or []), len(b or []))
        detail = {
            "metric": name,
            "differing_values": count,
            "first_frame": 0,
            "left": "absent" if a is None else f"{len(a)} frames",
            "right": "absent" if b is None else f"{len(b)} frames",
        }
        return count, detail
    bad = [index for index in range(len(a)) if a[index] != b[index]]
    if not bad:
        return 0, None
    first = bad[0]
    detail = {
        "metric": name,
        "differing_values": len(bad),
        "first_frame": first,
        "left": _fmt(a[first]),
        "right": _fmt(b[first]),
        "max_abs_diff": _fmt(_max_abs_diff(a, b)),
    }
    return len(bad), detail


def compare_scores(left: Scores, right: Scores) -> dict[str, Any]:
    """Exact comparison of two score sets; differing values are kept at `%.17g`."""
    names = sorted(set(left) | set(right))
    details: list[dict[str, Any]] = []
    total = sum(max(len(left.get(n, ())), len(right.get(n, ()))) for n in names)
    differing = 0
    for name in names:
        count, detail = _compare_series(name, left.get(name), right.get(name))
        differing += count
        if detail is not None:
            details.append(detail)
    return {
        "metrics": len(names),
        "values": total,
        "differing_values": differing,
        "differing_metrics": len(details),
        "details": details[:MAX_REPORTED_METRICS],
        "details_truncated": max(0, len(details) - MAX_REPORTED_METRICS),
    }


def status_of(cells: Sequence[Mapping[str, Any]]) -> str:
    """`identical`, `differing` or `error` over a list of fixture cells."""
    if any("error" in cell for cell in cells):
        return "error"
    return "differing" if any(c["differing_values"] for c in cells) else "identical"


def executed_levels(
    default_meta: Mapping[str, Any], scalar_meta: Mapping[str, Any]
) -> dict[str, Any]:
    """What the library says it executed in the default and the scalar-masked run
    (its provenance record, ADR-2073); empty when it gave none."""
    default = default_meta.get("provenance")
    scalar = scalar_meta.get("provenance")
    if not default or not scalar:
        return {}
    executed: dict[str, Any] = {"default_simd": default["simd"], "scalar_simd": scalar["simd"]}
    for key, name in (("digest", "provenance_digest"), ("version", "version"),
                      ("commit", "commit"), ("build_id", "build_id"), ("abi", "abi"),
                      ("conformance_mode", "conformance_mode")):  # fmt: skip
        if key in default:
            executed[name] = default[key]
    return executed


def run_dispatch_equivalence(
    vmaf: str,
    fixtures: Sequence[Mapping[str, Any]],
    *,
    timeout_seconds: float,
    runner: Runner = run_bounded,
) -> tuple[dict[str, Any], dict[str, tuple[Scores, Scores]]]:
    """Default dispatch against scalar on every fixture; also returns the raw scores."""
    cells: list[dict[str, Any]] = []
    raw: dict[str, tuple[Scores, Scores]] = {}
    for fixture in fixtures:
        cell: dict[str, Any] = {"fixture": fixture["id"]}
        try:
            default, default_meta = run_fixture_meta(
                vmaf, fixture, None, timeout_seconds=timeout_seconds, runner=runner
            )
            scalar, scalar_meta = run_fixture_meta(
                vmaf, fixture, SCALAR_CPUMASK, timeout_seconds=timeout_seconds, runner=runner
            )
        except (FixtureRunError, TimeoutError, RuntimeError, ValueError) as error:
            cell["error"] = str(error)
        else:
            raw[str(fixture["id"])] = (default, scalar)
            cell.update(compare_scores(default, scalar))
            cell["executed"] = executed_levels(default_meta, scalar_meta)
        cells.append(cell)
    return {"status": status_of(cells), "fixtures": cells}, raw
