#!/usr/bin/env python3
import json
import os
import subprocess

# Resolve the repo root so this script works from any worktree.
# Override with VMAF_TESTDATA to point at a different directory.
_repo_root = subprocess.run(
    ["git", "rev-parse", "--show-toplevel"],
    capture_output=True,
    text=True,
).stdout.strip() or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_default_testdata = os.path.join(_repo_root, "testdata")
os.chdir(os.environ.get("VMAF_TESTDATA", _default_testdata))

for res in ["576", "640", "720", "1080", "4k"]:
    cpu_file = "scores_cpu_" + res + ".json"
    sycl_file = "scores_sycl_a380_" + res + ".json"
    if not os.path.exists(cpu_file) or not os.path.exists(sycl_file):
        continue
    cpu = json.load(open(cpu_file))
    sycl = json.load(open(sycl_file))
    print("=== %s ===" % res)
    max_diff = 0
    max_frame = 0
    for i in range(min(len(cpu["frames"]), len(sycl["frames"]))):
        cv = cpu["frames"][i]["metrics"]["vmaf"]
        sv = sycl["frames"][i]["metrics"]["vmaf"]
        d = abs(cv - sv)
        if d > max_diff:
            max_diff = d
            max_frame = i
        if d > 1.0:
            print("  frame %d: CPU=%.6f SYCL=%.6f diff=%.6f" % (i, cv, sv, d))
    print("  max diff at frame %d: %.6f" % (max_frame, max_diff))
    print(
        "  pooled CPU=%.6f SYCL=%.6f"
        % (cpu["pooled_metrics"]["vmaf"]["mean"], sycl["pooled_metrics"]["vmaf"]["mean"])
    )
    print()
