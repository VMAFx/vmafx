#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Run the scoring contract test (#2155) on this build.

The same requests through the vmaf CLI, the C API (vmafx_score_contract) and
the scoring server (gRPC and POST /v1/score) must give the same score bit for
bit and the same provenance: cmd/vmafx-server/score_contract_test.go. This
driver points that Go test at this build and fails unless it really ran: a Go
test that skipped is reported as a skip (exit 77, reason printed), never as a
pass.

    run_score_contract.py ROOT VMAF_BIN CAPI LIBDIR
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

SKIP = 77
TIMEOUT = 1200  # seconds: compiling the server with cgo, then three cases
PAIR = ("src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv")


def environment(root: Path, vmaf: str, capi: str, libdir: str) -> dict[str, str]:
    env = dict(os.environ)
    env.update(
        {
            "VMAF_BIN": vmaf,
            "VMAFX_CONTRACT_CAPI": capi,
            "VMAFX_CONTRACT_YUV": str(root / "python" / "test" / "resource" / "yuv"),
            "VMAFX_CONTRACT_MODEL_DIR": str(root / "model"),
            "CGO_LDFLAGS": f"-L{libdir} -lvmaf -lm",
            "LD_LIBRARY_PATH": libdir + os.pathsep + env.get("LD_LIBRARY_PATH", ""),
            "GOFLAGS": "-p=4",
        }
    )
    return env


def main(argv: list[str]) -> int:
    root, vmaf, capi, libdir = Path(argv[1]), argv[2], argv[3], argv[4]
    go = shutil.which("go")
    if go is None:
        print("SKIP: the Go toolchain is not on PATH; the contract test runs the Go server")
        return SKIP
    yuv = root / "python" / "test" / "resource" / "yuv"
    missing = [name for name in PAIR if not (yuv / name).is_file()]
    if missing:
        print(f"SKIP: fixture(s) {missing} not in {yuv} (link python/test/resource/yuv)")
        return SKIP
    command = [go, "test", "-count=1", "-run", "^TestScoreContract$", "-v", "./cmd/vmafx-server/"]
    result = subprocess.run(  # noqa: S603 -- resolved go, fixed argv
        command,
        cwd=root,
        env=environment(root, vmaf, capi, libdir),
        capture_output=True,
        text=True,
        timeout=TIMEOUT,
        check=False,
    )
    print(result.stdout[-20000:])
    print(result.stderr[-5000:], file=sys.stderr)
    if result.returncode != 0:
        return 1
    if "--- PASS: TestScoreContract " not in result.stdout:
        print("FAIL: TestScoreContract did not run (skipped or filtered); see the output above")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
