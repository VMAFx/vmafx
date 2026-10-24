#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Stage-aware verdicts for the libvmaf_sycl zero-copy end-to-end harness.

``scripts/test/zerocopy-e2e.sh`` runs every case through three legs on the same
QSV-encoded pair and writes ``<clip>_<depth>bit__<case>.<leg>.{json,rc,err}``:

* ``cpu``  - software decode into ``libvmaf`` (the reference);
* ``host`` - software decode into ``libvmaf_sycl`` (host upload, SYCL twins);
* ``zc``   - QSV decode into ``libvmaf_sycl`` (zero-copy).

This module reads those files and decides, per case:

* ``host`` must equal ``cpu`` exactly (ADR-3360); otherwise ``NONEXACT host-vs-cpu``.
  The tolerance ``resolve_cell_tolerance()`` would grant is printed for
  information only and never applied. A case may declare a bound explicitly
  (``Case.cpu_bound``): only ``ciede`` does, with the parity gate's LIBM_TWINS
  cell (1e-9; a libm residual, ADR-1436). The PASS line then names the measured
  difference and the bound. Zero-copy against host upload is always exact.
  The harness runs every leg with ``score_fmt=%.17g`` so these are full-double
  comparisons, not six-decimal ones.
* A case whose stage (``PARITY_STAGE``) is at or below ``--stage`` must succeed
  on ``zc`` with every CPU metric present (``FAIL silent-drop`` otherwise) and
  equal to ``host`` (``FAIL zc-vs-host`` otherwise).
* A case of a later stage must fail loudly: non-zero exit and stderr naming the
  CPU feature or its SYCL twin. A success is ``FAIL unexpected-success`` and a
  failure that names nothing is ``FAIL unnamed-failure``. The comparator never
  matches message text beyond the feature names, so the wording may change
  between stages.

Stage 1 is what the luma-only zero-copy path admits (ADR-1688). Stages 2 and 3
need chroma on the device and the host-staging twins moved to the shared planes,
the post-1.0 zero-copy import of ADR-1685; until then their cases are refused
with ``-ENOTSUP`` naming the extractor, which is the loud failure checked here.

An absent metric in a successful zero-copy output is never a pass (T-12-07).

Host-upload reference cases (``Case.reference == "host"``): ``motion_uv`` is
``motion`` with ``motion_add_uv=true``. The CPU ``motion`` extractor has no such
option (only ``float_motion`` does), so there is no CPU leg; the harness runs the
host and zero-copy legs only and zero-copy must equal host upload exactly (and on
every ``--repeat`` run). No host-vs-cpu verdict exists for such a case.
``float_motion_uv`` covers the option against the CPU on the float extractor.

Usage::

    zerocopy_e2e_compare.py --stage {1,2,3} --dir DIR [--cases a,b,...]
    zerocopy_e2e_compare.py --list [--cases a,b,...]
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci.cross_backend_calibration import libm_pair_tolerance
from scripts.ci.cross_backend_parity_gate import (
    diff_frames,
    load_frames,
    missing_metrics,
    resolve_cell_tolerance,
)

LEGS = ("cpu", "host", "zc")

# CPU outputs that no SYCL twin writes on any input path (host upload included), so
# they cannot be a zero-copy regression. Measured on the A380 at 12-05: the SYCL
# ``motion`` twin omits the debug SAD score that the CPU, CUDA and HIP ``motion``
# write (docs/state.md T-GPU-MOTION-SAD-SCORE-NOT-EMITTED-2026-10-02); the parity
# gate's FEATURE_METRICS["motion"] leaves it out as well. Every other CPU metric
# must be present.
SYCL_TWIN_OMITTED: frozenset[str] = frozenset({"VMAF_integer_feature_motion_sad_score"})


@dataclass(frozen=True)
class Case:
    """One matrix row.

    ``kind`` is ``feature`` or ``model``; ``arg`` is the value of the filter's
    ``feature=`` / ``model=`` option (options after the name escaped with ``\\:``
    as docs/usage/ffmpeg.md shows); ``names`` are the strings a loud failure may
    use to name the case (CPU feature name or SYCL twin name); ``tolerance_key``
    is the feature handed to ``resolve_cell_tolerance`` for the info line.
    ``reference`` is ``cpu`` (the CPU leg is the reference and host must equal it)
    or ``host`` (no CPU leg: host upload is the only reference). ``cpu_bound`` is
    the declared absolute bound of host against CPU; 0.0 means exact.
    """

    kind: str
    arg: str
    names: tuple[str, ...]
    tolerance_key: str
    reference: str = "cpu"
    cpu_bound: float = 0.0


def _feature(
    name: str, opts: str = "", *aliases: str, reference: str = "cpu", cpu_bound: float = 0.0
) -> Case:
    arg = f"name={name}" + (f"\\:{opts}" if opts else "")
    return Case("feature", arg, (name, f"{name}_sycl", *aliases), name, reference, cpu_bound)


def _libm_bound(feature: str) -> float:
    """The parity gate's bound for a CPU-vs-SYCL cell that differs only in libm."""

    bound = libm_pair_tolerance(feature, "cpu", "sycl")
    if bound is None:
        raise SystemExit(f"{feature}: no LIBM_TWINS bound for sycl")
    return bound


CASES: dict[str, Case] = {
    # Stage 1: what the luma-only zero-copy path admits (ADR-1688).
    "vif": _feature("vif"),
    "adm": _feature("adm"),
    "motion": _feature("motion"),
    "motion_v2": _feature("motion_v2"),
    "cambi": _feature("cambi"),
    "float_moment": _feature("float_moment"),
    "psnr_luma": _feature("psnr", "enable_chroma=false"),
    "psnr_hvs_luma": _feature("psnr_hvs", "enable_chroma=false"),
    "model-vmaf_v0.6.1": Case("model", "version=vmaf_v0.6.1", ("vmaf_v0.6.1",), "vif"),
    # Stage 2: chroma import (post-1.0; refused on the luma-only path).
    "psnr": _feature("psnr"),
    "psnr_hvs": _feature("psnr_hvs"),
    # Integer motion with motion_add_uv: the CPU extractor lacks the option, so the
    # reference is host upload of motion_sycl.
    "motion_uv": _feature("motion", "motion_add_uv=true", reference="host"),
    # Stage 3: host-staging extractors on the shared planes (post-1.0; refused
    # on the luma-only path).
    "float_ssim": _feature("float_ssim"),
    "float_ms_ssim": _feature("float_ms_ssim"),
    "float_psnr": _feature("float_psnr"),
    "float_adm": _feature("float_adm"),
    "float_vif": _feature("float_vif"),
    "float_motion": _feature("float_motion"),
    "float_motion_uv": _feature("float_motion", "motion_add_uv=true"),
    "ssim": _feature("ssim", "", "integer_ssim"),
    # ciede_sycl is the CPU's arithmetic on fp32 pairs, so it differs from the CPU
    # extractor only where glibc's powf / fp64 functions round differently
    # (ADR-1436, T-CUDA-CIEDE-LIBM-RESIDUAL-2026-10-01): measured 1.1e-11 on one
    # src01 8-bit frame. The bound is the gate's LIBM_TWINS cell, shared, not a
    # second number. Zero-copy against host upload stays exact.
    "ciede": _feature("ciede", cpu_bound=_libm_bound("ciede")),
    "ssimulacra2": _feature("ssimulacra2"),
    "speed_chroma": _feature("speed_chroma"),
    "speed_temporal": _feature("speed_temporal"),
    "model-vmaf_float_v0.6.1": Case(
        "model",
        "version=vmaf_float_v0.6.1",
        ("vmaf_float_v0.6.1", "float_adm", "float_vif", "float_motion"),
        "float_adm",
    ),
}

# Stage from which zero-copy must match the CPU numerically.
PARITY_STAGE: dict[str, int] = {
    "vif": 1,
    "adm": 1,
    "motion": 1,
    "motion_v2": 1,
    "cambi": 1,
    "float_moment": 1,
    "psnr_luma": 1,
    "psnr_hvs_luma": 1,
    "model-vmaf_v0.6.1": 1,
    "psnr": 2,
    "psnr_hvs": 2,
    "motion_uv": 2,
    "float_ssim": 3,
    "float_ms_ssim": 3,
    "float_psnr": 3,
    "float_adm": 3,
    "float_vif": 3,
    "float_motion": 3,
    "float_motion_uv": 3,
    "ssim": 3,
    "ciede": 3,
    "ssimulacra2": 3,
    "speed_chroma": 3,
    "speed_temporal": 3,
    "model-vmaf_float_v0.6.1": 3,
}


@dataclass(frozen=True)
class Verdict:
    """``kind`` is PASS, FAIL or NONEXACT; ``label`` names the reason."""

    kind: str
    label: str
    detail: str = ""


def _leg_file(base: Path, ext: str) -> Path:
    """``<prefix>.<leg>`` plus ``ext``; ``with_suffix`` would eat dotted case ids."""

    return base.with_name(base.name + ext)


def _read_rc(base: Path) -> int | None:
    try:
        return int(_leg_file(base, ".rc").read_text().strip())
    except (OSError, ValueError):
        return None


def _read_err(base: Path) -> str:
    try:
        return _leg_file(base, ".err").read_text(errors="replace")
    except OSError:
        return ""


def _frames(base: Path) -> list[dict[str, Any]] | None:
    try:
        return load_frames(_leg_file(base, ".json"))
    except (OSError, ValueError):
        return None


def _metric_keys(frames: list[dict[str, Any]]) -> tuple[str, ...]:
    keys: set[str] = set()
    for frame in frames:
        keys.update(frame.get("metrics", {}))
    return tuple(sorted(keys - SYCL_TWIN_OMITTED))


def _worst(per_max: dict[str, float], per_mismatch: dict[str, int]) -> str:
    bad = sorted(((d, m) for m, d in per_max.items() if per_mismatch[m] or d > 0), reverse=True)
    return ", ".join(f"{m} max={d:.3e}" for d, m in bad[:5])


def _compare_exact(
    a: list[dict[str, Any]], b: list[dict[str, Any]], keys: tuple[str, ...]
) -> str | None:
    """None when ``a`` and ``b`` agree exactly on ``keys``, else a description."""

    if len(a) != len(b):
        return f"frame count {len(a)} vs {len(b)}"
    gone = missing_metrics(b, keys)
    if gone:
        return "missing " + ",".join(gone)
    per_max, per_mismatch = diff_frames(a, b, keys, 0.0)
    if any(per_mismatch.values()):
        return _worst(per_max, per_mismatch)
    return None


def _compare_bounded(
    a: list[dict[str, Any]], b: list[dict[str, Any]], keys: tuple[str, ...], bound: float
) -> str | None:
    """None when ``a`` and ``b`` agree within ``bound`` (absolute) on ``keys``."""

    if bound == 0.0:
        return _compare_exact(a, b, keys)
    if len(a) != len(b):
        return f"frame count {len(a)} vs {len(b)}"
    gone = missing_metrics(b, keys)
    if gone:
        return "missing " + ",".join(gone)
    per_max, per_mismatch = diff_frames(a, b, keys, bound)
    if any(per_mismatch.values()):
        return _worst(per_max, per_mismatch) + f" exceeds declared bound {bound:g}"
    return None


def _host_vs_cpu(case: Case, cpu: list[dict[str, Any]], host: list[dict[str, Any]]) -> str | None:
    if case.reference == "host":
        return None
    problem = _compare_bounded(cpu, host, _metric_keys(cpu), case.cpu_bound)
    if problem is None:
        return None
    tol, source = resolve_cell_tolerance(
        case.tolerance_key,
        fp16_features=(),
        calibration=None,
        gpu_id=None,
        backends=("cpu", "sycl"),
    )
    return f"{problem} tolerance(info)={tol:g} ({source}); applied {case.cpu_bound:g}"


def _with_bound_note(
    case: Case, cpu: list[dict[str, Any]], host: list[dict[str, Any]], verdict: Verdict
) -> Verdict:
    """Name the declared CPU bound and the measured difference on a bounded PASS."""

    if case.cpu_bound == 0.0 or verdict.kind != "PASS" or case.reference == "host":
        return verdict
    keys = _metric_keys(cpu)
    per_max, _ = diff_frames(cpu, host, keys, case.cpu_bound)
    worst = max(per_max.values(), default=0.0)
    note = f"host-vs-cpu max={worst:.3e} within declared bound {case.cpu_bound:g}"
    return Verdict("PASS", verdict.label, f"{verdict.detail} {note}".strip())


def _names_failure(case: Case, err: str) -> bool:
    return any(n in err for n in case.names)


def _loud_fail_verdict(case: Case, zc_rc: int, zc_err: str) -> Verdict:
    if zc_rc == 0:
        return Verdict("FAIL", "unexpected-success", "zero-copy succeeded before its stage")
    if not _names_failure(case, zc_err):
        tail = zc_err.strip().splitlines()[-1:] or ["<empty stderr>"]
        return Verdict("FAIL", "unnamed-failure", f"stderr names none of {case.names}: {tail[0]}")
    return Verdict("PASS", "loud-fail", f"rc={zc_rc}")


def _parity_verdict(
    zc_rc: int,
    zc_err: str,
    base: Path,
    cpu: list[dict[str, Any]],
    host: list[dict[str, Any]],
) -> Verdict:
    if zc_rc != 0:
        tail = zc_err.strip().splitlines()[-1:] or ["<empty stderr>"]
        return Verdict("FAIL", "zc-failed", f"rc={zc_rc}: {tail[0]}")
    zc = _frames(base)
    if zc is None:
        return Verdict("FAIL", "missing-leg", "zc output JSON unreadable")
    if len(zc) != len(host):
        return Verdict("FAIL", "zc-vs-host", f"frame count {len(host)} vs {len(zc)}")
    gone = missing_metrics(zc, _metric_keys(cpu))
    if gone:
        return Verdict("FAIL", "silent-drop", "absent from zero-copy output: " + ",".join(gone))
    problem = _compare_exact(host, zc, _metric_keys(cpu))
    if problem is not None:
        return Verdict("FAIL", "zc-vs-host", problem)
    return Verdict("PASS", "", f"frames={len(zc)} metrics={len(_metric_keys(cpu))}")


def _repeat_bases(d: Path, prefix: str) -> list[Path]:
    """Extra zero-copy runs (``--repeat``) as ``<prefix>.zc-r<k>``, in run order."""

    found = []
    for rc_file in d.glob(f"{prefix}.zc-r*.rc"):
        k = rc_file.name[len(prefix) + len(".zc-r") : -len(".rc")]
        if k.isdigit():
            found.append((int(k), d / f"{prefix}.zc-r{k}"))
    return [base for _, base in sorted(found)]


def _repeat_verdict(
    d: Path, prefix: str, cpu: list[dict[str, Any]], host: list[dict[str, Any]]
) -> Verdict | None:
    """FAIL when any repeated zero-copy run differs from host upload, else None.

    Zero-copy must be deterministic: every repeat is held to the same exact
    parity as the first run (sycl-zerocopy-cambi-nondeterminism)."""

    repeats = _repeat_bases(d, prefix)
    bad = []
    for base in repeats:
        rc = _read_rc(base)
        if rc is None:
            return Verdict("FAIL", "missing-leg", f"{base.name} rc file absent")
        v = _parity_verdict(rc, _read_err(base), base, cpu, host)
        if v.kind == "FAIL":
            bad.append(f"{base.name.rsplit('.', 1)[-1]}: {v.label} {v.detail}")
    if bad:
        runs = len(repeats) + 1
        return Verdict(
            "FAIL", "zc-nondeterministic", f"{len(bad)} of {runs} runs differ; " + "; ".join(bad)
        )
    return None


def _reference_legs(
    d: Path, prefix: str, case: Case
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]] | Verdict:
    """Frames of the CPU and host legs, or the verdict explaining why not.

    A host-reference case has no CPU leg: host upload stands in for both."""

    out: list[list[dict[str, Any]]] = []
    for leg in ("host",) if case.reference == "host" else ("cpu", "host"):
        base = d / f"{prefix}.{leg}"
        rc = _read_rc(base)
        if rc is None:
            return Verdict("FAIL", "missing-leg", f"{leg} rc file absent")
        if rc != 0:
            return Verdict("FAIL", f"{leg}-failed", f"rc={rc}")
        frames = _frames(base)
        if frames is None:
            return Verdict("FAIL", "missing-leg", f"{leg} output JSON unreadable")
        out.append(frames)
    return out[0], out[-1]


def evaluate_case(d: Path, prefix: str, case_id: str, stage: int) -> Verdict:
    """Verdict for one case. ``prefix`` is ``<clip>_<depth>bit__<case>``."""

    case = CASES[case_id]
    refs = _reference_legs(d, prefix, case)
    if isinstance(refs, Verdict):
        return refs
    cpu, host = refs
    zc_base = d / f"{prefix}.zc"
    zc_rc = _read_rc(zc_base)
    if zc_rc is None:
        return Verdict("FAIL", "missing-leg", "zc rc file absent")
    zc_err = _read_err(zc_base)
    if PARITY_STAGE[case_id] > stage:
        verdict = _loud_fail_verdict(case, zc_rc, zc_err)
    else:
        verdict = _parity_verdict(zc_rc, zc_err, zc_base, cpu, host)
        if verdict.kind != "FAIL":
            verdict = _repeat_verdict(d, prefix, cpu, host) or verdict
    if verdict.kind == "FAIL":
        return verdict
    nonexact = _host_vs_cpu(case, cpu, host)
    if nonexact is not None:
        return Verdict("NONEXACT", "host-vs-cpu", nonexact)
    return _with_bound_note(case, cpu, host, verdict)


def _discover(d: Path) -> list[tuple[str, str]]:
    """``(clip, depth)`` pairs that have at least one case file in ``d``."""

    found: set[tuple[str, str]] = set()
    for leg in ("cpu", "host"):
        for rc_file in d.glob(f"*__*.{leg}.rc"):
            head = rc_file.name.split("__", 1)[0]
            clip, _, depth = head.rpartition("_")
            found.add((clip, depth.removesuffix("bit")))
    return sorted(found)


def _parse_cases(raw: str | None) -> list[str]:
    if not raw:
        return list(PARITY_STAGE)
    ids = [c for c in raw.split(",") if c]
    unknown = [c for c in ids if c not in PARITY_STAGE]
    if unknown:
        raise SystemExit(f"unknown case id(s): {', '.join(unknown)}")
    return ids


def _format(clip: str, depth: str, case_id: str, v: Verdict) -> str:
    label = f" {v.label}" if v.label else ""
    detail = f" {v.detail}" if v.detail else ""
    return f"ZC-E2E {clip} {depth} {case_id} {v.kind}{label}{detail}"


def _run(args: argparse.Namespace) -> int:
    d = Path(args.dir)
    cases = _parse_cases(args.cases)
    counts = {"PASS": 0, "FAIL": 0, "NONEXACT": 0}
    for clip, depth in _discover(d):
        for case_id in cases:
            prefix = f"{clip}_{depth}bit__{case_id}"
            verdict = evaluate_case(d, prefix, case_id, args.stage)
            counts[verdict.kind] += 1
            print(_format(clip, depth, case_id, verdict))
    print(
        f"ZC-E2E SUMMARY stage={args.stage} pass={counts['PASS']} "
        f"fail={counts['FAIL']} nonexact={counts['NONEXACT']}"
    )
    ok = counts["PASS"] > 0 and counts["FAIL"] == 0 and counts["NONEXACT"] == 0
    return 0 if ok else 1


def _list_cases(raw: str | None) -> int:
    for case_id in _parse_cases(raw):
        case = CASES[case_id]
        print(f"{case_id}\t{case.kind}\t{case.arg}\t{PARITY_STAGE[case_id]}\t{case.reference}")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--stage", type=int, choices=(1, 2, 3))
    ap.add_argument("--dir")
    ap.add_argument("--cases", help="comma-separated case ids (default: all)")
    ap.add_argument(
        "--list", action="store_true", help="print id, kind, filter arg, stage, reference leg"
    )
    args = ap.parse_args(argv)
    if args.list:
        return _list_cases(args.cases)
    if args.stage is None or not args.dir:
        ap.error("--stage and --dir are required")
    return _run(args)


if __name__ == "__main__":
    sys.exit(main())
