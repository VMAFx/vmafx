#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

import json
import os
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
# Override with VMAF_TESTDATA to point at a different directory.
_repo_root_text = run_command(
    ["git", "rev-parse", "--show-toplevel"],
    capture_output=True,
    text=True,
).stdout.strip()
_repo_root = Path(_repo_root_text) if _repo_root_text else Path(__file__).resolve().parent.parent
_default_testdata = _repo_root / "testdata"
_testdata = Path(os.environ.get("VMAF_TESTDATA", _default_testdata))

for res in ["576", "640", "720", "1080", "4k"]:
    cpu_file = _testdata / f"scores_cpu_{res}.json"
    sycl_file = _testdata / f"scores_sycl_a380_{res}.json"
    if not cpu_file.exists() or not sycl_file.exists():
        continue
    with cpu_file.open(encoding="utf-8") as cpu_handle:
        cpu = json.load(cpu_handle)
    with sycl_file.open(encoding="utf-8") as sycl_handle:
        sycl = json.load(sycl_handle)
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
