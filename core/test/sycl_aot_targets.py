#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The default SYCL AOT targets and the sub-group sizes each accepts (ADR-1468).

icpx compiles every kernel ahead of time for each target of
``sycl_icpx_aot_targets`` (core/meson_options.txt). A kernel that requires a
sub-group size a target does not support fails for that target, and with it
its translation unit and the build:

    [lnl-m] error: in kernel '...': Kernel compiled with required subgroup
    size 8, which is unsupported on this platform

``SIZES_BY_FAMILY`` is what ocloc 26.35 accepts, measured with
``ocloc_accepts()`` on every target of the default list.
``test_sycl_aot_default_targets.py`` repeats the measurement where ocloc is
installed; ``test_sycl_sub_group_size_contract.py`` holds the sources to the
sizes every default target takes without a compiler.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OPTIONS = ROOT / "core" / "meson_options.txt"

# The sizes a kernel can require on an Intel GPU.
PROBED_SIZES = (8, 16, 32)

# Target family (the name up to its first hyphen) -> accepted sizes.
SIZES_BY_FAMILY: dict[str, tuple[int, ...]] = {
    "tgllp": (8, 16, 32),  # Xe-LP
    "adl": (8, 16, 32),
    "rpl": (8, 16, 32),
    "dg2": (8, 16, 32),  # Xe-HPG
    "acm": (8, 16, 32),
    "mtl": (8, 16, 32),  # Xe-LPG
    "arl": (8, 16, 32),
    "lnl": (16, 32),  # Xe2: no SIMD-8
    "bmg": (16, 32),
}

# One kernel that requires a sub-group size, in OpenCL C: ocloc compiles it
# for a target in well under a second.
PROBE_SOURCE = (
    "#pragma OPENCL EXTENSION cl_intel_required_subgroup_size : enable\n"
    "__attribute__((intel_reqd_sub_group_size({size})))\n"
    "kernel void k(global float *a) {{ a[get_global_id(0)] += 1.0f; }}\n"
)

_OPTION = re.compile(r"option\(\s*'sycl_icpx_aot_targets'\s*,.*?value\s*:\s*'([^']*)'", re.S)


def default_targets(options_text: str | None = None) -> list[str]:
    """The targets of the option's default, in its order."""
    text = OPTIONS.read_text(encoding="utf-8") if options_text is None else options_text
    match = _OPTION.search(text)
    if not match:
        raise ValueError("core/meson_options.txt: no sycl_icpx_aot_targets default")
    return [name.strip() for name in match.group(1).split(",") if name.strip()]


def family(target: str) -> str:
    """`dg2-g11` -> `dg2`."""
    return target.split("-", 1)[0]


def supported_sizes(target: str) -> tuple[int, ...] | None:
    """The sizes the target's family accepts, or None for an unknown family."""
    return SIZES_BY_FAMILY.get(family(target))


def common_sizes(targets: list[str]) -> set[int]:
    """The sizes every one of `targets` accepts; a KeyError names an unknown one."""
    sizes = set(PROBED_SIZES)
    for target in targets:
        accepted = supported_sizes(target)
        if accepted is None:
            raise KeyError(target)
        sizes &= set(accepted)
    return sizes


def ocloc_accepts(ocloc: str, target: str, size: int, workdir: Path) -> bool:
    """Whether ocloc compiles a kernel that requires `size` for `target`."""
    source = workdir / f"sub_group_{size}.cl"
    source.write_text(PROBE_SOURCE.format(size=size), encoding="utf-8")
    out_dir = str(workdir / f"out_{target}_{size}")
    result = subprocess.run(  # noqa: S603 -- the caller's resolved ocloc, fixed arguments, no shell
        [ocloc, "compile", "-file", str(source), "-device", target, "-q", "-out_dir", out_dir],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )
    return result.returncode == 0
