#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""JSON comparisons and summaries for ``scripts/dev/rc3-home-gpu-retest.sh``.

Each subcommand prints one line for the kit's summary and returns 0 (within
the row's bound), 1 (outside it) or 2 (the input could not be read):

    compare REF TEST TOL [KEYS]     max abs diff per frame (KEYS: comma list)
    backends JSON BACKEND TWIN...   the twins ran on BACKEND (feature_backends)
    image CPU_JSON GPU_JSON TOL     oneAPI image: backend_used and pooled VMAF
    msframe SHORT LONG NS...        median ms/frame of (short, long) run pairs
    speedsum OUTPUT                 condense scripts/dev/speed_gpu_parity.py output
    buildopts BUILD_DIR BACKEND     the meson build enables BACKEND
    table SUMMARY_TSV               the kit's summary as a Markdown table
"""

from __future__ import annotations

import json
import math
import re
import statistics
import sys
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import Any

BACKEND_OPTIONS = {
    "cuda": ("enable_cuda",),
    "hip": ("enable_hip", "enable_hipcc"),
    "sycl": ("enable_sycl",),
}
PARITY_LINE = re.compile(r"^(\S+) (\S+): bit-identical (\d+)/(\d+), max abs diff (\S+)$")
TIMING_LINE = re.compile(r"^(\S+) (\S+): cpu(\d+) (\S+) ms/frame, (\w+) (\S+) ms/frame$")
MAX_ERROR_TEXT = 160


def load(path: str) -> dict[str, Any]:
    """The first JSON object in ``path`` (``--output /dev/stdout`` may add text)."""
    text = Path(path).read_text(encoding="utf-8")
    start = text.find("{")
    if start < 0:
        raise ValueError(f"{path}: no JSON object")
    payload = json.JSONDecoder().raw_decode(text[start:])[0]
    if not isinstance(payload, dict):
        raise ValueError(f"{path}: not a JSON object")
    return payload


def frame_metrics(path: str) -> list[dict[str, Any]]:
    return [frame["metrics"] for frame in load(path)["frames"]]


def number(value: float | None) -> float:
    """The rows' one-liners compare ``(x or 0) - (y or 0)``; keep that for null."""
    return 0.0 if value is None else float(value)


def delta(a: float | None, b: float | None) -> float:
    """Absolute difference; NaN counts as infinitely far (never within a bound)."""
    value = abs(number(a) - number(b))
    return math.inf if math.isnan(value) else value


def compare(ref: str, test: str, tol: str, keys: str = "") -> int:
    """Frame-by-frame max abs diff of every metric (or of ``keys``)."""
    a, b = frame_metrics(ref), frame_metrics(test)
    if not a or len(a) != len(b):
        print(f"frame count differs: {len(a)} vs {len(b)}")
        return 2
    wanted = [key for key in keys.split(",") if key] or sorted(a[0])
    missing = sorted({key for key in wanted if key not in a[0] or key not in b[0]})
    names = [key for key in wanted if key not in missing]
    worst, worst_key, same = 0.0, "", 0
    for x, y in zip(a, b, strict=True):
        deltas = {key: delta(x.get(key), y.get(key)) for key in names}
        same += all(value == 0.0 for value in deltas.values())
        for key, value in deltas.items():
            if value > worst:
                worst, worst_key = value, key
    text = f"max {worst:.3g}" + (f" ({worst_key})" if worst_key else "")
    text += f", {same}/{len(a)} frames identical"
    if missing:
        text += "; missing " + ", ".join(missing)
    print(text)
    return 1 if missing or worst > float(tol) else 0


def backends(path: str, backend: str, *twins: str) -> int:
    """Every twin appears in ``feature_backends`` on ``backend``."""
    ran = {
        (entry.get("extractor"), entry.get("backend"))
        for entry in load(path).get("feature_backends", [])
    }
    listed = ", ".join(f"{name}:{where}" for name, where in sorted(ran)) or "none"
    absent = [twin for twin in twins if (twin, backend) not in ran]
    if absent:
        print(f"feature_backends lacks {', '.join(absent)} (has {listed})")
        return 1
    print(f"feature_backends: {listed}")
    return 0


def image(cpu_path: str, gpu_path: str, tol: str) -> int:
    """The image's SYCL run reports sycl and a pooled VMAF within ``tol`` of the CPU."""
    cpu, gpu = load(cpu_path), load(gpu_path)
    a = float(cpu["pooled_metrics"]["vmaf"]["mean"])
    b = float(gpu["pooled_metrics"]["vmaf"]["mean"])
    used = gpu.get("backend_used")
    print(f"backend_used {used}, vmaf mean cpu {a:.6f} vs {b:.6f}, |diff| {abs(a - b):.3g}")
    return 0 if used == "sycl" and abs(a - b) <= float(tol) else 1


def msframe(short: str, long: str, *samples: str) -> int:
    """Median over run pairs of (t(long) - t(short)) / (long - short), in ms.

    ``samples`` alternate short and long wall times in nanoseconds, as the kit
    records them: the rows' "median(t(22) - t(2)) / 20".
    """
    frames = int(long) - int(short)
    values = [int(sample) for sample in samples]
    if frames <= 0 or not values or len(values) % 2:
        raise ValueError("msframe needs LONG > SHORT and (short, long) sample pairs")
    pairs = [(values[i + 1] - values[i]) / 1e6 / frames for i in range(0, len(values), 2)]
    print(f"{statistics.median(pairs):.2f}")
    return 0


def speedsum(path: str) -> int:
    """One line from ``speed_gpu_parity.py`` output: worst output per fixture,
    the identical-frame count of that output, ms/frame and any error."""
    worst: dict[str, tuple[float, str, int, int]] = {}
    timings: list[str] = []
    errors: list[str] = []
    for raw in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if match := PARITY_LINE.match(line):
            fixture, output, exact, total, value = match.groups()
            if fixture not in worst or float(value) > worst[fixture][0]:
                worst[fixture] = (float(value), output, int(exact), int(total))
        elif match := TIMING_LINE.match(line):
            fixture, feature, threads, cpu_ms, backend, gpu_ms = match.groups()
            timings.append(f"{fixture} {feature} {backend} {gpu_ms} / cpu{threads} {cpu_ms}")
        elif line.startswith("speed_gpu_parity:"):
            errors.append(line.split(":", 1)[1].strip()[:MAX_ERROR_TEXT])
    parts = [
        f"{fixture}: max {value:.3g} ({output}), {exact}/{total} frames identical"
        for fixture, (value, output, exact, total) in worst.items()
    ]
    if timings:
        parts.append("ms/frame " + "; ".join(timings))
    parts.extend(f"error: {error}" for error in errors)
    print("; ".join(parts) or "no parity output")
    return 0


def buildopts(build_dir: str, backend: str) -> int:
    """0 when the meson build in ``build_dir`` enables ``backend``."""
    path = Path(build_dir) / "meson-info" / "intro-buildoptions.json"
    options = {entry["name"]: entry["value"] for entry in json.loads(path.read_text("utf-8"))}
    enabled = all(options.get(name) is True for name in BACKEND_OPTIONS[backend])
    print(f"{build_dir}: {backend} {'enabled' if enabled else 'not enabled'}")
    return 0 if enabled else 1


def table(path: str) -> int:
    """The kit's summary TSV (row, backend, result, notes) as Markdown."""
    print("| Row | Backend | Result | Key numbers |")
    print("|---|---|---|---|")
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        row, backend, status, notes = line.split("\t", 3)
        notes = notes.replace("|", "\\|")
        print(f"| `{row}` | {backend} | {status} | {notes} |")
    return 0


COMMANDS: dict[str, Callable[..., int]] = {
    "compare": compare,
    "backends": backends,
    "image": image,
    "msframe": msframe,
    "speedsum": speedsum,
    "buildopts": buildopts,
    "table": table,
}


def main(argv: Sequence[str]) -> int:
    if not argv or argv[0] not in COMMANDS:
        print(f"usage: rc3_retest_helpers.py {{{','.join(COMMANDS)}}} ARGS...")
        return 2
    try:
        return COMMANDS[argv[0]](*argv[1:])
    except (OSError, KeyError, ValueError, TypeError, IndexError) as error:
        print(f"{type(error).__name__}: {error}")
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
