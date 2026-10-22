#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Reference-pair verification report (run-all-tests.sh [REPORT] + `vmaf-selftest`).

Instead of a pass/fail black box, this runs the canonical Netflix reference
pairs through the `vmaf` CLI and prints, per pair: the reference value, the
actual CPU (and SYCL) values, the delta, the tolerance, and the verdict.

╔══════════════════════════════════════════════════════════════════════════╗
║ INVARIANT — STRICT NETFLIX REFERENCE (product-owner rule, 2026-07-09)      ║
║                                                                            ║
║ The golden REFERENCE is ALWAYS the Netflix golden value, sourced verbatim  ║
║ from python/test/quality_runner_test.py (GLOBAL RULE 1 / CLAUDE.md §8 —    ║
║ the immutable numerical ground truth). It is NEVER a fork-captured         ║
║ "baseline" or the tool's own output. The per-pair TOLERANCE is Netflix's   ║
║ OWN `places=N` for that assertion (places=2 -> 5e-3, places=4 -> 5e-5),    ║
║ not an arbitrary widening. Deviating from this rule — substituting a       ║
║ baseline, loosening a tolerance beyond Netflix's places — is NOT ALLOWED   ║
║ without the EXPRESS consent of the product owner. See the NETFLIX_VMAF     ║
║ table below; every entry cites its quality_runner_test.py line + places.   ║
║                                                                            ║
║ src01 provenance: the reference is Netflix upstream's CURRENT value        ║
║ 76.66783025 @ places=4, not the older 76.66890519623612 @ places=2. The    ║
║ latter predates Netflix commit a44e5e611 ("bugfix for edge mirroring" in   ║
║ integer_motion), which corrected motion2 3.8953519 -> 3.8943597 and        ║
║ re-tightened the assertion to places=4. The fork's code already produces   ║
║ the corrected value; the golden sync of quality_runner_test.py, a change   ║
║ of its own, puts it in the fork's file. Product-owner approved 2026-07-09. ║
╚══════════════════════════════════════════════════════════════════════════╝

The Cross-backend section is a SEPARATE, fork-internal parity check (does the
SYCL GPU path match the CPU path, per the ADR-0214 places=4 contract) — there
the anchor is the CPU result BY DEFINITION of a parity gate, not a golden
reference. It never claims a Netflix reference.

Env: VMAF_BIN (default /src/vmafx/build/tools/vmaf), YUV_DIR
(default python/test/resource/yuv), REPORT_NO_SYCL=1 to skip SYCL (CPU-only
hosts). Exit 0 iff every row is within tolerance.
"""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

VMAF = os.environ.get("VMAF_BIN", "/src/vmafx/build/tools/vmaf")
YUV = Path(os.environ.get("YUV_DIR", "python/test/resource/yuv"))
NO_SYCL = os.environ.get("REPORT_NO_SYCL", "0") == "1"

GRN, RED, C0 = "\033[1;32m", "\033[1;31m", "\033[0m"

# Canonical Netflix pairs. reference + tol are Netflix ground truth, NOT a fork
# baseline (see the INVARIANT above). `tol` is Netflix's own places for that
# VMAF_score assertion: places=2 -> 5e-3, places=4 -> 5e-5.
# (label, ref_yuv, dist_yuv, w, h, netflix_vmaf, tol, source)
PAIRS = [
    (
        "src01 hrc00->hrc01 576x324",
        "src01_hrc00_576x324.yuv",
        "src01_hrc01_576x324.yuv",
        576,
        324,
        76.66783025,
        5e-5,
        "quality_runner_test.py:152 (places=4; Netflix a44e5e611 motion edge-mirror fix)",
    ),
    (
        "checkerboard 1px 1920x1080",
        "checkerboard_1920_1080_10_3_0_0.yuv",
        "checkerboard_1920_1080_10_3_1_0.yuv",
        1920,
        1080,
        35.06866714286451,
        5e-5,
        "quality_runner_test.py:384 (places=4)",
    ),
    (
        "checkerboard 10px 1920x1080",
        "checkerboard_1920_1080_10_3_0_0.yuv",
        "checkerboard_1920_1080_10_3_10_0.yuv",
        1920,
        1080,
        7.985898744818505,
        5e-5,
        "quality_runner_test.py:382 (places=4)",
    ),
]

# Cross-backend parity feature set (SYCL vs CPU), (json_key, tolerance).
# CPU is the parity anchor per ADR-0214 places=4 — a backend-parity gate, NOT a
# golden reference (which is Netflix-only, above).
XBACKEND_METRICS = [
    ("integer_adm2", 1e-4),
    ("integer_adm3", 1e-4),
    ("integer_vif_scale0", 1e-4),
    ("integer_motion2", 1e-4),
    ("vmaf", 1e-4),
]


def run(ref: str, dist: str, w: int, h: int, backend: str) -> dict[str, Any] | None:
    """Run vmaf for one pair on one backend; return pooled_metrics or None."""
    out = Path(tempfile.gettempdir()) / ("reference_report_%s.json" % backend)
    cmd = [
        VMAF,
        "--backend",
        backend,
        "-r",
        str(YUV / ref),
        "-d",
        str(YUV / dist),
        "--width",
        str(w),
        "--height",
        str(h),
        "--pixel_format",
        "420",
        "--bitdepth",
        "8",
        # The Netflix reference values are for vmaf_v0.6.1; pin it, since the
        # CLI default model is not v0.6.1 on this branch.
        "--model",
        "version=vmaf_v0.6.1",
        "--output",
        str(out),
        "--json",
    ]
    # cmd is built from the fixed VMAF binary path and the in-file PAIRS table.
    r = subprocess.run(cmd, capture_output=True, text=True, check=False)  # noqa: S603
    if r.returncode != 0:
        sys.stderr.write(
            "  [report] %s backend failed (%d): %s\n"
            % (backend, r.returncode, r.stderr.strip()[:200])
        )
        return None
    try:
        with out.open() as f:
            pooled: dict[str, Any] = json.load(f)["pooled_metrics"]
            return pooled
    except (OSError, ValueError, KeyError) as e:
        sys.stderr.write("  [report] parse failed: %s\n" % e)
        return None


def cell(v: float | None) -> str:
    return "%.6f" % v if v is not None else "  n/a"


def verdict(delta: float | None, tol: float) -> tuple[str, bool]:
    ok = delta is not None and delta <= tol
    return (GRN + "PASS" + C0) if ok else (RED + "FAIL" + C0), ok


def golden_table(gpu: bool) -> bool:
    """Netflix golden: VMAF vs the Netflix reference (CPU + SYCL). True if all pass."""
    all_ok = True
    print("\n  Netflix golden — VMAF vs Netflix reference (CPU%s)" % (" + SYCL" if gpu else ""))
    hdr = "  %-28s %-14s %-13s %-13s %-9s %-6s %s"
    print(hdr % ("pair", "Netflix ref", "CPU", "SYCL", "delta", "tol", "verdict"))
    for label, ref, dist, w, h, refv, tol, src in PAIRS:
        mc = run(ref, dist, w, h, "cpu")
        ms = run(ref, dist, w, h, "sycl") if gpu else None
        cv = mc["vmaf"]["mean"] if (mc and "vmaf" in mc) else None
        sv = ms["vmaf"]["mean"] if (ms and "vmaf" in ms) else None
        cd = abs(cv - refv) if cv is not None else None
        # The Netflix golden is a CPU contract: the golden gate runs the CPU
        # backend. SYCL correctness is verified SEPARATELY by the cross-backend
        # parity table (SYCL vs CPU @ 1e-4, ADR-0214), NOT against the
        # tighter golden tol — a SYCL result within its parity envelope but
        # outside 5e-5 of the Netflix ref is CORRECT, not a golden failure.
        # So the golden verdict is CPU-vs-Netflix; SYCL is shown for info only.
        vtxt, ok = verdict(cd, tol)
        all_ok = all_ok and ok
        print(
            hdr
            % (
                label,
                "%.6f" % refv,
                cell(cv),
                (cell(sv) if gpu else "  -"),
                ("%.1e" % cd if cd is not None else "n/a"),
                "%.0e" % tol,
                vtxt,
            )
        )
        print("  %-28s   ^ %s" % ("", src))
    return all_ok


def parity_table() -> bool:
    """Cross-backend parity: SYCL vs CPU per feature (ADR-0214). True if all pass."""
    all_ok = True
    print(
        "\n  Cross-backend parity — SYCL vs CPU (ADR-0214 places=4; CPU is "
        "the parity anchor, not a golden ref)   src01 576x324"
    )
    hdr2 = "  %-18s %-13s %-13s %-9s %-6s %s"
    print(hdr2 % ("feature", "CPU", "SYCL", "delta", "tol", "verdict"))
    _label, ref, dist, w, h = PAIRS[0][:5]
    cpu = run(ref, dist, w, h, "cpu")
    sycl = run(ref, dist, w, h, "sycl")
    for key, tol in XBACKEND_METRICS:
        cv = cpu[key]["mean"] if (cpu and key in cpu) else None
        sv = sycl[key]["mean"] if (sycl and key in sycl) else None
        d = abs(cv - sv) if (cv is not None and sv is not None) else None
        vtxt, ok = verdict(d, tol)
        all_ok = all_ok and ok
        print(
            hdr2
            % (
                key,
                cell(cv),
                cell(sv),
                ("%.1e" % d if d is not None else "n/a"),
                "%.0e" % tol,
                vtxt,
            )
        )
    return all_ok


def main() -> int:
    gpu = not NO_SYCL
    all_ok = golden_table(gpu)
    if gpu:
        all_ok = parity_table() and all_ok
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
