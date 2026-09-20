#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Quick benchmark: 3 runs per resolution, report best fps."""

import os
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from testdata._command import run_command
else:
    try:
        from testdata._command import run_command
    except ModuleNotFoundError:
        from _command import run_command

# Resolve the repo root so this script works from any worktree.
# Override VMAF_TESTDATA / VMAF_BIN to point at alternate fixtures or binary.
_repo_root_text = run_command(
    ["git", "rev-parse", "--show-toplevel"],
    capture_output=True,
    text=True,
).stdout.strip()
_repo_root = Path(_repo_root_text) if _repo_root_text else Path(__file__).resolve().parent.parent
basedir = Path(os.environ.get("VMAF_TESTDATA", _repo_root / "testdata"))
os.chdir(basedir)
vmaf_bin = os.environ.get("VMAF_BIN", "/usr/local/bin/vmaf")
runs = 3

resolutions = [
    ("576x324", "576"),
    ("640x480", "640"),
    ("1280x720", "720"),
    ("1920x1080", "1080"),
    ("3840x2160", "4k"),
]

env = os.environ.copy()
env["LD_LIBRARY_PATH"] = "/usr/local/lib"

for dims, _tag in resolutions:
    w, h = dims.split("x")
    ref = "ref_%s_48f.yuv" % dims
    dis = "dis_%s_48f.yuv" % dims
    if not Path(ref).exists() or not Path(dis).exists():
        continue
    cmd = [
        vmaf_bin,
        "-r",
        ref,
        "-d",
        dis,
        "-w",
        w,
        "-h",
        h,
        "-p",
        "420",
        "-b",
        "8",
        "-m",
        "version=vmaf_v0.6.1",
        "--json",
        "-o",
        "/dev/null",
        "--no_cuda",
    ]
    fps_list = []
    for _ in range(runs):
        t0 = time.time()
        result = run_command(cmd, capture_output=True, text=True, env=env)
        elapsed = time.time() - t0
        if result.returncode != 0:
            detail = (result.stderr or result.stdout or "no diagnostic").strip().splitlines()
            print(f"{dims}: FAILED: {detail[-1]}", file=sys.stderr)
            fps_list = []
            break
        fps = 48.0 / elapsed
        fps_list.append(fps)
    if not fps_list:
        continue
    best = max(fps_list)
    avg = sum(fps_list) / len(fps_list)
    print(
        "%s: best=%.1f avg=%.1f fps  (%s)"
        % (dims, best, avg, ", ".join("%.1f" % f for f in fps_list))
    )
