#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Window scores of the live harness equal the offline CLI (RC4 WP4, #2238).

Runs ``test_vmafx_window_live`` (textures imported at 60 fps, windows of
0.2 s polled from another thread) with ``VMAFX_WINDOW_JSON`` set, runs the
``vmaf`` CLI on the same Netflix 576x324 pair at ``--precision max``, and
pools the CLI's per-frame scores over each window with the engine's
arithmetic (``pool_accumulate`` / ``pool_reduce`` / ``vmaf_percentile`` in
``core/src/libvmaf.c`` and ``core/src/percentile.h``: running sums in frame
order, no reassociation). Every one of the eight methods of every window
(the model and each of its features) must be the same double.

Usage: test_vmafx_window_cli.py <test_vmafx_window_live> <vmaf> <yuv dir>.
Exits 77 when the fixture pair is missing.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REF = "src01_hrc00_576x324.yuv"
DIST = "src01_hrc01_576x324.yuv"
MODEL = "vmaf_v0.6.1"
SKIP = 77

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "ci"))
from vmafx_window_pooling import cli_name, pooled  # noqa: E402 -- path set above


def run(cmd: list[str], env: dict[str, str]) -> None:
    result = subprocess.run(  # noqa: S603 -- the test's own build outputs, argument list, no shell
        cmd, env=env, capture_output=True, text=True, timeout=600, check=False
    )
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"{cmd[0]} exited {result.returncode}")


def main(argv: list[str]) -> int:
    harness, cli, yuv = argv[1], argv[2], Path(argv[3])
    if not (yuv / REF).is_file() or not (yuv / DIST).is_file():
        print(f"skip: fixtures {REF}, {DIST} not in {yuv}")
        return SKIP
    with tempfile.TemporaryDirectory() as tmp:
        windows_path = Path(tmp) / "windows.jsonl"
        cli_path = Path(tmp) / "cli.json"
        env = dict(os.environ, VMAFX_WINDOW_JSON=str(windows_path))
        run([harness], env)
        run(
            [
                cli,
                "-r",
                str(yuv / REF),
                "-d",
                str(yuv / DIST),
                "-w",
                "576",
                "-h",
                "324",
                "-p",
                "420",
                "-b",
                "8",
                "--model",
                f"version={MODEL}",
                "--precision",
                "max",
                "--json",
                "-o",
                str(cli_path),
                "-q",
            ],
            dict(os.environ),
        )
        lines = windows_path.read_text(encoding="utf-8").splitlines()
        frames = json.loads(cli_path.read_text(encoding="utf-8"))["frames"]
    windows = [json.loads(line) for line in lines]
    checked = 0
    for window in windows:
        first, n = window["first"], window["n_frames"]
        key = cli_name(window["target"])
        scores = [frame["metrics"][key] for frame in frames[first : first + n]]
        expected = pooled(scores)
        for slot in range(1, 9):
            if window["value"][slot] != expected[slot]:
                print(
                    f"FAIL {key} [{first}, {first + n - 1}] pool {slot}: "
                    f"window {window['value'][slot]!r}, CLI {expected[slot]!r}"
                )
                return 1
            checked += 1
    if not windows or checked != 8 * len(windows):
        print("FAIL: no windows checked")
        return 1
    print(f"{len(windows)} windows, {checked} pooled values equal to the CLI's frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
