#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Raw inputs of different sizes through the ``vmaf`` CLI (Netflix/vmaf 94f3ff0dd8).

``--width_ref`` / ``--height_ref`` / ``--width_dist`` / ``--height_dist`` size
each raw input and default to ``--width`` / ``--height``. The CLI no longer
rejects different sizes itself: libvmaf does, unless the models scale their
inputs to a display resolution, and a distorted picture whose size differs
from the picture pool's is allocated on its own.

Cases:

* positive: the Netflix 576x324 pair sized with the per-input options scores;
* positive: a 576x324 reference against a 288x162 distorted input scores with
  a model whose conversion target names ``resolution.display`` 1080;
* negative: the same pair with ``vmaf_v0.6.1`` (no scaling) fails in libvmaf;
* negative: a reference width without a distorted width (and no ``-w``) and a
  width of 0 are refused by the CLI.

Usage: test_cli_input_sizes.py <vmaf> <yuv dir> <model dir>.
Exits 77 when the fixture pair is missing.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

REF = "src01_hrc00_576x324.yuv"
DIST = "src01_hrc01_576x324.yuv"
W, H = 576, 324
FRAMES = 2
SKIP = 77


def run(vmaf: str, args: list[str]) -> subprocess.CompletedProcess[str]:
    """Run the CLI with ``args``; never raises on a non-zero exit."""
    return subprocess.run(  # noqa: S603 -- the test's own build output, argument list, no shell
        [vmaf, *args], capture_output=True, text=True, timeout=300, check=False
    )


def half_size(src: Path, dst: Path) -> None:
    """Write the first FRAMES 4:2:0 frames of ``src`` decimated by 2 to ``dst``."""
    frame = W * H * 3 // 2
    data = src.read_bytes()[: frame * FRAMES]
    out = bytearray()
    for f in range(FRAMES):
        base = f * frame
        for off, pw, ph in ((0, W, H), (W * H, W // 2, H // 2), (W * H * 5 // 4, W // 2, H // 2)):
            plane = data[base + off : base + off + pw * ph]
            for y in range(0, ph, 2):
                out += plane[y * pw : (y + 1) * pw : 2]
    dst.write_bytes(bytes(out))


def display_model(model_dir: Path, dst: Path) -> None:
    """vmaf_v0.6.1 with a BT.709 conversion target at a 1080 display resolution."""
    model = json.loads((model_dir / "vmaf_v0.6.1.json").read_text(encoding="utf-8"))
    model["model_dict"]["conversion_target"] = {
        "colorspace": {"range": "limited", "primaries": "bt709", "trc": "bt709", "matrix": "bt709"},
        "resolution": {"display": 1080},
    }
    dst.write_text(json.dumps(model), encoding="utf-8")


def colour_args() -> list[str]:
    args = []
    for side in ("ref", "dist"):
        args += [f"--color_range_{side}", "limited", f"--color_primaries_{side}", "bt709"]
        args += [f"--color_trc_{side}", "bt709", f"--color_matrix_{side}", "bt709"]
    return args


def expect(cond: bool, what: str, proc: subprocess.CompletedProcess[str]) -> None:
    if not cond:
        sys.stderr.write(f"FAIL: {what}\nexit {proc.returncode}\n{proc.stderr}\n")
        sys.exit(1)


def case_same_sizes(vmaf: str, ref: Path, dist: Path, tail: list[str]) -> None:
    """Positive: the per-input options with equal sizes score."""
    sizes = ["--width_ref", str(W), "--height_ref", str(H)]
    sizes += ["--width_dist", str(W), "--height_dist", str(H)]
    proc = run(vmaf, ["-r", str(ref), "-d", str(dist), *sizes, *tail])
    expect(proc.returncode == 0, "same sizes through the per-input options", proc)


def case_different_sizes(vmaf: str, sizes: list[str], tail: list[str], model: Path) -> None:
    """Positive with a display-resolution model, negative without scaling."""
    proc = run(vmaf, [*sizes, *tail, "-m", f"path={model}", *colour_args()])
    expect(proc.returncode == 0, "different sizes with a display-resolution model", proc)
    frames = json.loads(Path(tail[-1]).read_text(encoding="utf-8"))["frames"]
    expect(len(frames) == FRAMES and "vmaf" in frames[0]["metrics"], "a vmaf score per frame", proc)

    proc = run(vmaf, [*sizes, *tail, "-m", "version=vmaf_v0.6.1"])
    expect(proc.returncode != 0, "different sizes without scaling must fail", proc)
    expect(
        "dimensions do not match" not in proc.stderr,
        "the CLI must leave the size check to libvmaf",
        proc,
    )


def case_cli_refusals(vmaf: str, ref: Path, dist: Path, tail: list[str]) -> None:
    """Negative: a reference width alone and a width of 0 are refused by the CLI."""
    proc = run(vmaf, ["-r", str(ref), "-d", str(dist), "--width_ref", str(W), "-h", str(H), *tail])
    expect(
        proc.returncode == 1 and "--width_ref and --width_dist" in proc.stderr,
        "a reference width alone must be refused",
        proc,
    )
    pair = ["-r", str(ref), "-d", str(dist), "-w", str(W), "-h", str(H)]
    proc = run(vmaf, [*pair, "--width_dist", "0", *tail])
    expect(proc.returncode != 0, "a width of 0 must be refused", proc)


def main() -> int:
    vmaf, yuv_dir, model_dir = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    ref, dist = yuv_dir / REF, yuv_dir / DIST
    if not ref.is_file() or not dist.is_file():
        print(f"SKIP: fixture pair missing under {yuv_dir}")
        return SKIP
    with tempfile.TemporaryDirectory() as tmp:
        tmpd = Path(tmp)
        small = tmpd / "dist_288x162.yuv"
        half_size(dist, small)
        model = tmpd / "display_1080.json"
        display_model(model_dir, model)
        tail = ["-p", "420", "-b", "8", "--frame_cnt", str(FRAMES), "-q", "--json", "-o"]
        tail.append(str(tmpd / "out.json"))
        case_same_sizes(vmaf, ref, dist, tail)
        sizes = ["-r", str(ref), "-d", str(small), "-w", str(W), "-h", str(H)]
        sizes += ["--width_dist", str(W // 2), "--height_dist", str(H // 2)]
        case_different_sizes(vmaf, sizes, tail, model)
        case_cli_refusals(vmaf, ref, dist, tail)
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
