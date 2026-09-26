#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Generate SYCL scores on the current GPU for all resolutions.
Usage: python3 run_sycl_scores.py [gpu_tag]
Example: python3 run_sycl_scores.py a380
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

RESOLUTIONS = [
    ("576x324", "576"),
    ("640x480", "640"),
    ("1280x720", "720"),
    ("1920x1080", "1080"),
    ("3840x2160", "4k"),
]

DEFAULT_MODEL = "version=vmaf_v0.6.1"


def build_vmaf_cmd(
    vmaf_bin: str,
    ref: str | Path,
    dis: str | Path,
    width: str | int,
    height: str | int,
    model: str,
    output_path: str | Path,
    backend: str = "sycl",
    device: int | None = None,
) -> list[str]:
    """Construct command to run vmaf CLI with the selected backend."""
    cmd = [
        str(vmaf_bin),
        "-r",
        str(ref),
        "-d",
        str(dis),
        "-w",
        str(width),
        "-h",
        str(height),
        "-p",
        "420",
        "-b",
        "8",
        "-m",
        model,
        "--json",
        "-o",
        str(output_path),
        "--backend",
        backend,
    ]
    if device is not None:
        cmd.extend(["--sycl_device", str(device)])
    return cmd


def _vmaf_library_dir(vmaf_bin: str | Path) -> Path | None:
    """Resolve the library directory that belongs to the selected CLI."""
    if override := os.environ.get("VMAF_LIB_DIR"):
        return Path(override).expanduser().resolve()

    binary = Path(vmaf_bin).expanduser()
    if binary.is_absolute() or binary.parent != Path("."):
        binary = binary.resolve()
        build_lib = binary.parent.parent / "src"
        if build_lib.is_dir() and any(build_lib.glob("libvmaf.*")):
            return build_lib
        if binary == Path("/usr/local/bin/vmaf"):
            return Path("/usr/local/lib")
    return None


def prepare_vmaf_env(vmaf_bin: str | Path) -> dict[str, str]:
    """Bind a vmaf CLI invocation to its matching libvmaf when discoverable."""
    env = os.environ.copy()
    if library_dir := _vmaf_library_dir(vmaf_bin):
        library = str(library_dir)
        existing = [part for part in env.get("LD_LIBRARY_PATH", "").split(":") if part]
        env["LD_LIBRARY_PATH"] = ":".join(
            [library, *(part for part in existing if part != library)]
        )
    return env


def prepare_sycl_env(vmaf_bin: str | Path) -> dict[str, str]:
    """Construct an exact-artifact environment for SYCL vmaf on Intel Arc."""
    env = prepare_vmaf_env(vmaf_bin)
    if "ONEAPI_DEVICE_SELECTOR" not in env:
        env["ONEAPI_DEVICE_SELECTOR"] = "level_zero:gpu"
    return env


def run_single_resolution(
    vmaf_bin: str,
    dims: str,
    tag: str,
    gpu_tag: str,
    model: str = DEFAULT_MODEL,
    basedir: Path | None = None,
) -> bool:
    """Run VMAF scoring for a single resolution and write output JSON."""
    if basedir is None:
        basedir = Path(__file__).resolve().parent

    w, h = dims.split("x")
    ref = basedir / f"ref_{dims}_48f.yuv"
    dis = basedir / f"dis_{dims}_48f.yuv"
    if not ref.exists() or not dis.exists():
        print(f"SKIP {dims} — files not found")
        return False

    out = basedir / f"scores_sycl_{gpu_tag}_{tag}.json"
    print(f"\n=== {dims} SYCL on {gpu_tag} ===")

    cmd = build_vmaf_cmd(
        vmaf_bin=vmaf_bin,
        ref=ref,
        dis=dis,
        width=w,
        height=h,
        model=model,
        output_path=out,
        backend="sycl",
    )
    print("  " + " ".join(cmd))

    t0 = time.time()
    env = prepare_sycl_env(vmaf_bin)
    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    elapsed = time.time() - t0

    if result.returncode != 0:
        print(f"  FAILED (exit {result.returncode})")
        print(f"  stderr: {result.stderr[:500]}")
        return False

    with open(out, "r", encoding="utf-8") as f:
        j = json.load(f)
    pooled = j["pooled_metrics"]["vmaf"]["mean"]
    nf = len(j["frames"])
    fps = nf / elapsed if elapsed > 0 else 0
    j["fps"] = round(fps, 2)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(j, f, indent=2)

    print(f"  {nf} frames, pooled={pooled:.6f}, fps={fps:.2f}, time={elapsed:.2f}s")
    compare_vs_cpu(basedir, tag, j)
    return True


def compare_vs_cpu(basedir: Path, tag: str, sycl_data: dict) -> float | None:
    """Compare SYCL frame scores against CPU golden reference if present."""
    cpu_file = basedir / f"scores_cpu_{tag}.json"
    if not cpu_file.exists():
        return None

    with open(cpu_file, "r", encoding="utf-8") as f:
        cpu = json.load(f)
    cpu_scores = [fr["metrics"]["vmaf"] for fr in cpu["frames"]]
    sycl_scores = [fr["metrics"]["vmaf"] for fr in sycl_data["frames"]]
    max_diff = max(abs(a - b) for a, b in zip(cpu_scores, sycl_scores))
    status = "OK" if max_diff < 0.001 else "WARN"
    print(f"  vs CPU max diff: {max_diff:.9f}  {status}")
    return max_diff


def main(argv: list[str] | None = None) -> int:
    """CLI entrypoint."""
    if argv is None:
        argv = sys.argv[1:]
    gpu_tag = argv[0] if argv else "a380"
    vmaf_bin = os.environ.get("VMAF_BIN", "/usr/local/bin/vmaf")
    basedir = Path(__file__).resolve().parent

    for dims, tag in RESOLUTIONS:
        run_single_resolution(
            vmaf_bin=vmaf_bin,
            dims=dims,
            tag=tag,
            gpu_tag=gpu_tag,
            basedir=basedir,
        )

    print("\nDone!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
