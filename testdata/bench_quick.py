#!/usr/bin/env python3
"""Quick benchmark: 3 runs per resolution, report best fps."""

import json
import os
import subprocess
import sys
import time

# Resolve the repo root so this script works from any worktree.
# Override VMAF_TESTDATA / VMAF_BIN to point at alternate fixtures or binary.
_repo_root = subprocess.run(
    ["git", "rev-parse", "--show-toplevel"],
    capture_output=True,
    text=True,
).stdout.strip() or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
basedir = os.environ.get("VMAF_TESTDATA", os.path.join(_repo_root, "testdata"))
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

for dims, tag in resolutions:
    w, h = dims.split("x")
    ref = "ref_%s_48f.yuv" % dims
    dis = "dis_%s_48f.yuv" % dims
    if not os.path.exists(ref):
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
    for i in range(runs):
        t0 = time.time()
        subprocess.run(cmd, capture_output=True, env=env)
        elapsed = time.time() - t0
        fps = 48.0 / elapsed
        fps_list.append(fps)
    best = max(fps_list)
    avg = sum(fps_list) / len(fps_list)
    print(
        "%s: best=%.1f avg=%.1f fps  (%s)"
        % (dims, best, avg, ", ".join("%.1f" % f for f in fps_list))
    )
