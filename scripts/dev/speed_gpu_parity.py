#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Frame-by-frame parity and timing of a SpEED GPU twin against the CPU extractor.

ADR-1358 makes the SYCL twins bit-identical to the CPU extractor; the CUDA and
HIP twins are to follow. For each fixture (the Netflix 576x324 pair, 48 frames,
and BBB 3840x2160, 50 frames) and each of ``speed_chroma`` / ``speed_temporal``
this runs the CPU extractor and the ``<feature>_<backend>`` twin at
``--precision max``, reports the bit-identical frame count and the maximum
absolute difference per output, then times both as ``(t(22) - t(2)) / 20``
milliseconds per frame, the median of ``--reps`` repetitions.

A GPU twin must be requested by its registered name: ``--feature speed_chroma``
resolves to the CPU extractor whatever ``--backend`` says.

Usage::

    python3 scripts/dev/speed_gpu_parity.py --backend cuda
    ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/dev/speed_gpu_parity.py --backend sycl

Exit status: 0 when every output of every frame is identical, 1 when any
differs, 2 on a usage or run error.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import tempfile
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import CommandFailed, CommandTimedOut
from scripts.lib.safe_subprocess import run as run_command

FEATURES = ("speed_chroma", "speed_temporal")
BACKENDS = ("cuda", "sycl", "hip", "metal")
SHORT_RUN = 2
LONG_RUN = 22
RUN_TIMEOUT_SECONDS = 1800.0


@dataclass(frozen=True)
class Fixture:
    """One reference/distorted pair of 8-bit 4:2:0 raw YUV files."""

    name: str
    ref: Path
    dis: Path
    width: int
    height: int
    frames: int


@dataclass(frozen=True)
class Difference:
    """Per-output comparison of two per-frame score lists."""

    exact: int
    total: int
    max_abs: float


def fixtures(netflix_dir: Path, bbb_dir: Path) -> list[Fixture]:
    """The two fixtures of the ADR-1358 contract."""
    return [
        Fixture(
            "576x324",
            netflix_dir / "src01_hrc00_576x324.yuv",
            netflix_dir / "src01_hrc01_576x324.yuv",
            576,
            324,
            48,
        ),
        Fixture(
            "3840x2160",
            bbb_dir / "ref_3840x2160_200f.yuv",
            bbb_dir / "dis_3840x2160_200f.yuv",
            3840,
            2160,
            50,
        ),
    ]


def vmaf_command(
    vmaf: Path,
    fixture: Fixture,
    feature: str,
    backend: str | None,
    frames: int,
    output: Path,
    threads: int,
) -> list[str]:
    """The CLI invocation; ``backend=None`` runs the CPU extractor."""
    command = [
        str(vmaf),
        "-r",
        str(fixture.ref),
        "-d",
        str(fixture.dis),
        "-w",
        str(fixture.width),
        "-h",
        str(fixture.height),
        "-p",
        "420",
        "-b",
        "8",
        "--frame_cnt",
        str(frames),
        "--precision",
        "max",
        "--no_prediction",
        "-o",
        str(output),
        "--json",
        "-q",
    ]
    if backend is None:
        return [*command, "--backend", "cpu", "--threads", str(threads), "--feature", feature]
    return [*command, "--backend", backend, "--feature", f"{feature}_{backend}"]


def load_frames(path: Path) -> list[dict[str, float]]:
    """Per-frame metric dictionaries of a ``--json`` output file."""
    payload = json.loads(path.read_text(encoding="utf-8"))
    return [frame["metrics"] for frame in payload["frames"]]


def compare(cpu: list[dict[str, float]], gpu: list[dict[str, float]]) -> dict[str, Difference]:
    """Compare every output of every frame. Mismatched frame counts or output
    sets are an error, not a difference: the twin did not run the same job."""
    if not cpu or len(cpu) != len(gpu):
        raise ValueError(f"frame count differs: cpu={len(cpu)} gpu={len(gpu)}")
    names = sorted(cpu[0])
    if names != sorted(gpu[0]):
        raise ValueError(f"outputs differ: cpu={names} gpu={sorted(gpu[0])}")
    result: dict[str, Difference] = {}
    for name in names:
        deltas = [abs(c[name] - g[name]) for c, g in zip(cpu, gpu, strict=True)]
        exact = sum(1 for delta in deltas if delta == 0.0)
        result[name] = Difference(exact, len(deltas), max(deltas))
    return result


def ms_per_frame(short_seconds: float, long_seconds: float) -> float:
    """Startup-free cost of one frame from a SHORT_RUN and a LONG_RUN."""
    return (long_seconds - short_seconds) * 1000.0 / (LONG_RUN - SHORT_RUN)


def run_vmaf(command: Sequence[str]) -> None:
    """Run one CLI invocation; a non-zero exit raises CommandFailed."""
    run_command(
        command,
        allowed_executables=(command[0],),
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=RUN_TIMEOUT_SECONDS,
    )


def wall_seconds(command: Sequence[str]) -> float:
    start = time.perf_counter()
    run_vmaf(command)
    return time.perf_counter() - start


def timed(args: argparse.Namespace, fixture: Fixture, feature: str, backend: str | None) -> float:
    """Median ms/frame over ``args.reps`` (short, long) pairs."""
    samples = []
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "t.json"
        for _ in range(args.reps):
            short = wall_seconds(
                vmaf_command(args.vmaf, fixture, feature, backend, SHORT_RUN, out, args.threads)
            )
            long = wall_seconds(
                vmaf_command(args.vmaf, fixture, feature, backend, LONG_RUN, out, args.threads)
            )
            samples.append(ms_per_frame(short, long))
    return statistics.median(samples)


def check_pair(args: argparse.Namespace, fixture: Fixture, feature: str) -> bool:
    """Run, compare and report one fixture/feature pair; True when identical."""
    with tempfile.TemporaryDirectory() as tmp:
        cpu_out = Path(tmp) / "cpu.json"
        gpu_out = Path(tmp) / "gpu.json"
        for backend, out in ((None, cpu_out), (args.backend, gpu_out)):
            command = vmaf_command(
                args.vmaf, fixture, feature, backend, fixture.frames, out, args.threads
            )
            run_vmaf(command)
        result = compare(load_frames(cpu_out), load_frames(gpu_out))
    identical = True
    for name, diff in result.items():
        identical = identical and diff.exact == diff.total
        print(
            f"{fixture.name} {name}: bit-identical {diff.exact}/{diff.total}, "
            f"max abs diff {diff.max_abs:.3e}"
        )
    return identical


def parse(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--backend", required=True, choices=BACKENDS)
    parser.add_argument("--vmaf", type=Path, default=Path("build/tools/vmaf"))
    parser.add_argument("--netflix-dir", type=Path, default=Path("python/test/resource/yuv"))
    parser.add_argument("--bbb-dir", type=Path, default=Path("testdata/bbb"))
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--no-timing", action="store_true")
    args = parser.parse_args(argv)
    if args.reps < 1 or args.threads < 1:
        parser.error("--reps and --threads must be at least 1")
    return args


def main(argv: Sequence[str]) -> int:
    args = parse(argv)
    identical = True
    try:
        for fixture in fixtures(args.netflix_dir, args.bbb_dir):
            for feature in FEATURES:
                identical = check_pair(args, fixture, feature) and identical
                if args.no_timing:
                    continue
                cpu_ms = timed(args, fixture, feature, None)
                gpu_ms = timed(args, fixture, feature, args.backend)
                print(
                    f"{fixture.name} {feature}: cpu{args.threads} {cpu_ms:.2f} ms/frame, "
                    f"{args.backend} {gpu_ms:.2f} ms/frame"
                )
    except (OSError, ValueError, CommandFailed, CommandTimedOut) as error:
        print(f"speed_gpu_parity: {error}", file=sys.stderr)
        return 2
    return 0 if identical else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
