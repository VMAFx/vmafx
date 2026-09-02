#!/usr/bin/env python3
"""Performance benchmark over BBB raw/encoded fixtures via FFmpeg.

The harness is intentionally operator-facing, not a CI gate. It defaults to
the in-tree BBB raw fixtures and lets callers opt into the external 4K MP4
decode path with ``--bbb-mp4-ref`` / ``VMAF_BBB_MP4_REF``. Missing optional
fixtures are skipped by default so one unavailable local file does not block
the raw-YUV performance run.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from collections.abc import Sequence
from pathlib import Path
from typing import Any

BASEDIR = Path(__file__).resolve().parent
DEFAULT_FFMPEG = os.environ.get("VMAF_FFMPEG", "ffmpeg")
DEFAULT_RUNS = int(os.environ.get("VMAF_BENCH_RUNS", "3"))
DEFAULT_TIMEOUT_S = float(os.environ.get("VMAF_BENCH_TIMEOUT_S", "1200"))


def build_tests(base_dir: Path, *, bbb_mp4_ref: Path | None = None) -> list[dict[str, Any]]:
    """Return the benchmark test descriptions.

    The first two raw-YUV tests are in-tree fixtures. The MP4 decode test is
    opt-in because the source file is large and intentionally not committed.
    """
    tests: list[dict[str, Any]] = [
        {
            "name": "BBB 1080p 48f (YUV)",
            "ref": base_dir / "ref_1920x1080_48f.yuv",
            "dis": base_dir / "dis_1920x1080_48f.yuv",
            "width": 1920,
            "height": 1080,
            "frames": 48,
            "pix_fmt": "yuv420p",
            "raw": True,
            "required": False,
        },
        {
            "name": "BBB 4K 200f (YUV)",
            "ref": base_dir / "bbb" / "ref_3840x2160_200f.yuv",
            "dis": base_dir / "bbb" / "dis_3840x2160_200f.yuv",
            "width": 3840,
            "height": 2160,
            "frames": 200,
            "pix_fmt": "yuv420p",
            "raw": True,
            "required": True,
        },
    ]
    if bbb_mp4_ref is not None:
        tests.append(
            {
                "name": "BBB 4K MP4 500f (decode+vmaf)",
                "ref": bbb_mp4_ref,
                "dis": base_dir / "bbb" / "dis_crf35.mp4",
                "width": 3840,
                "height": 2160,
                "frames": 500,
                "pix_fmt": None,
                "raw": False,
                "required": False,
            }
        )
    return tests


def backend_definitions(*, sycl_device: str) -> list[dict[str, Any]]:
    """Return backend definitions with the requested SYCL render node."""
    return [
        {
            "name": "cpu",
            "init_args": [],
            "lavfi": "[0:v][1:v]libvmaf=log_path={log}:log_fmt=json:model=version=vmaf_v0.6.1",
        },
        {
            "name": "cuda",
            "init_args": ["-init_hw_device", "cuda=cu", "-filter_hw_device", "cu"],
            "lavfi": (
                "[0:v]hwupload[dis];[1:v]hwupload[ref];"
                "[dis][ref]libvmaf_cuda=log_path={log}:log_fmt=json:model=version=vmaf_v0.6.1"
            ),
        },
        {
            "name": "sycl",
            "init_args": [
                "-init_hw_device",
                f"vaapi=va:{sycl_device}",
                "-init_hw_device",
                "qsv=qsv@va",
                "-filter_hw_device",
                "qsv",
            ],
            "lavfi": (
                "[0:v]hwupload=extra_hw_frames=128[dis];"
                "[1:v]hwupload=extra_hw_frames=128[ref];"
                "[dis][ref]libvmaf_sycl=log_path={log}:log_fmt=json:model=version=vmaf_v0.6.1"
            ),
            "env_extra": {"LIBVA_DRIVER_NAME": "iHD"},
        },
    ]


def select_backends(
    backends: Sequence[dict[str, Any]], selected: Sequence[str]
) -> list[dict[str, Any]]:
    """Filter backend definitions by name, preserving canonical order."""
    requested = set(selected)
    if "all" in requested:
        return list(backends)
    known = {backend["name"] for backend in backends}
    unknown = sorted(requested.difference(known))
    if unknown:
        raise ValueError(f"unknown backend(s): {', '.join(unknown)}")
    return [backend for backend in backends if backend["name"] in requested]


def missing_paths(test: dict[str, Any]) -> list[Path]:
    """Return missing input paths for ``test``."""
    return [Path(test[key]) for key in ("ref", "dis") if not Path(test[key]).exists()]


def available_tests(tests: Sequence[dict[str, Any]], *, require_all: bool) -> list[dict[str, Any]]:
    """Drop tests with missing optional inputs, or raise when required."""
    available: list[dict[str, Any]] = []
    missing_required: list[str] = []
    for test in tests:
        missing = missing_paths(test)
        if not missing:
            available.append(test)
            continue
        if require_all or bool(test.get("required", False)):
            joined = ", ".join(str(path) for path in missing)
            missing_required.append(f"{test['name']}: {joined}")
    if missing_required:
        raise FileNotFoundError("missing benchmark fixture(s): " + "; ".join(missing_required))
    return available


def _raw_inputs(test: dict[str, Any]) -> list[str]:
    """FFmpeg input args for raw YUV."""
    size = f"{test['width']}x{test['height']}"
    return [
        "-f",
        "rawvideo",
        "-pix_fmt",
        str(test["pix_fmt"]),
        "-s",
        size,
        "-i",
        str(test["dis"]),
        "-f",
        "rawvideo",
        "-pix_fmt",
        str(test["pix_fmt"]),
        "-s",
        size,
        "-i",
        str(test["ref"]),
    ]


def _mp4_inputs(test: dict[str, Any]) -> list[str]:
    """FFmpeg input args for encoded files."""
    return ["-i", str(test["dis"]), "-i", str(test["ref"])]


def build_ffmpeg_command(
    test: dict[str, Any],
    backend: dict[str, Any],
    *,
    log_path: Path,
    ffmpeg: Path,
) -> list[str]:
    """Build the FFmpeg command for one benchmark run."""
    inputs = _raw_inputs(test) if bool(test["raw"]) else _mp4_inputs(test)
    lavfi = str(backend["lavfi"]).format(log=log_path)
    cmd = [
        str(ffmpeg),
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        *backend["init_args"],
        *inputs,
        "-lavfi",
        lavfi,
    ]
    if not bool(test["raw"]) and test.get("frames"):
        cmd += ["-frames:v", str(test["frames"])]
    cmd += ["-f", "null", "-"]
    return cmd


def build_env(backend: dict[str, Any], *, ld_library_path: str | None) -> dict[str, str]:
    """Build the subprocess environment for one backend."""
    env = os.environ.copy()
    if ld_library_path is not None:
        env["LD_LIBRARY_PATH"] = ld_library_path
    elif "LD_LIBRARY_PATH" not in env:
        env["LD_LIBRARY_PATH"] = "/usr/local/lib"
    if "env_extra" in backend:
        env.update(backend["env_extra"])
    return env


def run_vmaf(
    test: dict[str, Any],
    backend: dict[str, Any],
    *,
    log_path: Path,
    ffmpeg: Path,
    timeout_s: float,
    ld_library_path: str | None,
) -> tuple[float | None, int, float, str | None]:
    """Run one FFmpeg VMAF invocation. Returns (pooled, nframes, elapsed, error)."""
    cmd = build_ffmpeg_command(test, backend, log_path=log_path, ffmpeg=ffmpeg)
    env = build_env(backend, ld_library_path=ld_library_path)
    start = time.time()
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout_s,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return None, 0, time.time() - start, f"TIMEOUT ({timeout_s:.0f}s)"
    elapsed = time.time() - start

    if result.returncode != 0:
        return None, 0, elapsed, result.stderr[-500:]

    try:
        data = json.loads(log_path.read_text(encoding="utf-8"))
        pooled = float(data["pooled_metrics"]["vmaf"]["mean"])
        nframes = len(data["frames"])
        return pooled, nframes, elapsed, None
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        return None, 0, elapsed, str(exc)


def _print_banner(args: argparse.Namespace) -> None:
    print("╔══════════════════════════════════════════════════════════════════════════╗")
    print("║              VMAF Performance Benchmark — Real Resolutions             ║")
    print("╚══════════════════════════════════════════════════════════════════════════╝")
    print(f"  FFmpeg : {args.ffmpeg}")
    print(f"  Runs   : {args.runs} per backend")
    print()


def _print_test_list(tests: Sequence[dict[str, Any]]) -> None:
    for test in tests:
        missing = missing_paths(test)
        state = (
            "available" if not missing else "missing: " + ", ".join(str(path) for path in missing)
        )
        print(f"{test['name']}: {state}")


def _run_one_test(
    test: dict[str, Any],
    backends: Sequence[dict[str, Any]],
    *,
    args: argparse.Namespace,
) -> dict[str, Any]:
    results: dict[str, Any] = {}
    nframes = test["frames"] or "auto"
    print(f"{'━' * 78}")
    print(f"  {test['name']}   ({test['width']}×{test['height']}, {nframes} frames)")
    print(f"{'━' * 78}")

    for backend in backends:
        name = str(backend["name"])
        sys.stdout.write(f"  {name:>12} : ")
        sys.stdout.flush()
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
            log_path = Path(tmp.name)

        pooled, frame_count, first_time, err = run_vmaf(
            test,
            backend,
            log_path=log_path,
            ffmpeg=args.ffmpeg,
            timeout_s=args.timeout_s,
            ld_library_path=args.ld_library_path,
        )

        if err is not None or pooled is None:
            print(f"FAILED — {str(err)[:120]}")
            results[name] = {"error": str(err)[:200]}
            log_path.unlink(missing_ok=True)
            continue

        times = [first_time]
        for _ in range(args.runs - 1):
            _, _, elapsed, retry_error = run_vmaf(
                test,
                backend,
                log_path=log_path,
                ffmpeg=args.ffmpeg,
                timeout_s=args.timeout_s,
                ld_library_path=args.ld_library_path,
            )
            if retry_error is None:
                times.append(elapsed)

        best = min(times)
        avg = sum(times) / len(times)
        best_fps = frame_count / best
        avg_fps = frame_count / avg
        print(
            f"VMAF {pooled:8.4f}  |  {best_fps:7.1f} fps best  "
            f"{avg_fps:7.1f} fps avg  ({best:.2f}s best of {len(times)})"
        )
        results[name] = {
            "pooled": pooled,
            "nframes": frame_count,
            "best_fps": best_fps,
            "avg_fps": avg_fps,
            "best_time": best,
            "times": times,
        }
        log_path.unlink(missing_ok=True)
    print()
    return results


def _print_summary(
    tests: Sequence[dict[str, Any]],
    backends: Sequence[dict[str, Any]],
    all_results: dict[str, dict[str, Any]],
) -> None:
    print()
    print(
        "╔══════════════════════════════════════════════════════════════════════════════════════╗"
    )
    print("║                              PERFORMANCE SUMMARY                                   ║")
    print(
        "╠══════════════════════════════════════════════════════════════════════════════════════╣"
    )

    for test in tests:
        test_results = all_results[test["name"]]
        nframes = next(
            (value["nframes"] for value in test_results.values() if "nframes" in value), "?"
        )
        print(f"║  {test['name']:<40} ({nframes} frames){'':>24}║")
        print("╟──────────────┬──────────┬───────────┬───────────┬──────────────────────────────╢")
        print(
            f"║ {'Backend':>12} │ {'VMAF':>8} │ {'Best FPS':>9} │ {'Avg FPS':>9} │ "
            f"{'Best Time':>9}  {'Speedup':>8}        ║"
        )
        print("╟──────────────┼──────────┼───────────┼───────────┼──────────────────────────────╢")

        cpu_time = None
        if "cpu" in test_results and "best_time" in test_results["cpu"]:
            cpu_time = float(test_results["cpu"]["best_time"])

        for backend in backends:
            name = str(backend["name"])
            result = test_results.get(name)
            if not result or "error" in result:
                print(f"║ {name:>12} │ {'FAIL':>8} │ {'':>9} │ {'':>9} │ {'':>9}  {'':>8}        ║")
                continue

            speedup = ""
            if cpu_time and "best_time" in result:
                speedup = f"{cpu_time / float(result['best_time']):.2f}×"
            print(
                f"║ {name:>12} │ {result['pooled']:>8.4f} │ {result['best_fps']:>9.1f} │ "
                f"{result['avg_fps']:>9.1f} │ {result['best_time']:>8.2f}s  "
                f"{speedup:>8}        ║"
            )

        print("╚══════════════╧══════════╧═══════════╧═══════════╧══════════════════════════════╝")
        print()


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", type=Path, default=Path(DEFAULT_FFMPEG), help="FFmpeg binary")
    parser.add_argument("--base-dir", type=Path, default=BASEDIR, help="testdata directory")
    parser.add_argument(
        "--bbb-mp4-ref",
        type=Path,
        default=Path(os.environ["VMAF_BBB_MP4_REF"]) if "VMAF_BBB_MP4_REF" in os.environ else None,
        help="optional BBB 4K MP4 reference for the decode+VMAF test",
    )
    parser.add_argument(
        "--backend",
        action="append",
        choices=("all", "cpu", "cuda", "sycl"),
        default=None,
        help="backend to run; repeatable (default: all)",
    )
    parser.add_argument("--runs", type=int, default=DEFAULT_RUNS, help="timing runs per backend")
    parser.add_argument(
        "--timeout-s", type=float, default=DEFAULT_TIMEOUT_S, help="per-run timeout"
    )
    parser.add_argument(
        "--sycl-device",
        default=os.environ.get("VMAF_SYCL_DEVICE", "/dev/dri/renderD130"),
        help="VAAPI render node for the SYCL/QSV import path",
    )
    parser.add_argument(
        "--ld-library-path",
        default=os.environ.get("VMAF_LD_LIBRARY_PATH"),
        help="LD_LIBRARY_PATH for FFmpeg/libvmaf; default preserves caller env or /usr/local/lib",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="JSON output path (default: <base-dir>/perf_benchmark_results.json)",
    )
    parser.add_argument(
        "--require-all",
        action="store_true",
        help="fail when any configured test fixture is missing instead of skipping optional tests",
    )
    parser.add_argument(
        "--list-tests",
        action="store_true",
        help="print configured tests and fixture availability, then exit",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print commands that would run, then exit",
    )
    args = parser.parse_args(argv)
    if args.runs < 1:
        parser.error("--runs must be >= 1")
    if args.timeout_s <= 0:
        parser.error("--timeout-s must be > 0")
    if args.backend is None:
        args.backend = ["all"]
    if args.output is None:
        args.output = args.base_dir / "perf_benchmark_results.json"
    return args


def main(argv: Sequence[str] | None = None) -> int:
    """CLI entry point."""
    args = parse_args(argv)
    tests = build_tests(args.base_dir, bbb_mp4_ref=args.bbb_mp4_ref)
    backends = select_backends(backend_definitions(sycl_device=args.sycl_device), args.backend)

    if args.list_tests:
        _print_test_list(tests)
        return 0

    try:
        tests = available_tests(tests, require_all=args.require_all)
    except FileNotFoundError as exc:
        sys.stderr.write(f"{exc}\n")
        return 1
    if not tests:
        sys.stderr.write("no benchmark tests available\n")
        return 1

    if args.dry_run:
        for test in tests:
            for backend in backends:
                command = build_ffmpeg_command(
                    test,
                    backend,
                    log_path=Path("{log}.json"),
                    ffmpeg=args.ffmpeg,
                )
                print(" ".join(command))
        return 0

    _print_banner(args)
    all_results: dict[str, dict[str, Any]] = {}
    for test in tests:
        all_results[test["name"]] = _run_one_test(test, backends, args=args)

    _print_summary(tests, backends, all_results)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(all_results, indent=2), encoding="utf-8")
    print(f"Results saved to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
