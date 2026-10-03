# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""`vmaf-tester-report`: the one command of the VMAFx tester image.

Runs offline, prints one JSON report on stdout and a short summary on stderr.
Exit 0 only when every check that ran agrees and none was skipped; 1 when a check
failed; 2 when a check was skipped (a maintainer-only option).
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from . import __version__
from .hw_equiv import run_dispatch_equivalence
from .hw_facts import collect_host_facts, read_build_info
from .hw_gate import run_metal_gate
from .hw_metal import run_metal_equivalence_raw
from .hw_reference import generate_reference, run_reference_equivalence
from .hw_rows import evaluate_rows, load_row_map
from .hw_suites import run_golden_gate, run_unit_tests, summary_line
from .safe_process import run_bounded

# 2: the Metal gate and the state-row map (ADR-1496).
SCHEMA_VERSION = "2"
DEFAULT_ROOT = "/opt/vmafx"
CHECKS = ("dispatch", "reference", "metal", "gate", "unit", "golden")
FIXTURE_TIMEOUT_SECONDS = 3600.0
GATE_TIMEOUT_SECONDS = 3 * 3600.0
UNIT_TIMEOUT_SECONDS = 900.0
GOLDEN_TIMEOUT_SECONDS = 4 * 3600.0
UNHASHED_FIELDS = ("report_sha256", "note")


def sha256_file(path: Path) -> str | None:
    """Hex SHA-256 of a file, or None when it cannot be read."""
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
    except OSError:
        return None
    return digest.hexdigest()


def report_digest(report: Mapping[str, Any]) -> str:
    """SHA-256 over the canonical JSON of the report without its unhashed fields."""
    body = {key: value for key, value in report.items() if key not in UNHASHED_FIELDS}
    canonical = json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(canonical.encode("ascii")).hexdigest()


def load_fixtures(root: Path) -> list[dict[str, Any]]:
    """Fixtures baked into the image; paths are resolved against the image root."""
    data = json.loads((root / "image" / "fixtures.json").read_text(encoding="utf-8"))
    fixtures = []
    for entry in data["fixtures"]:
        fixture = dict(entry)
        fixture["ref"] = str(root / entry["ref"])
        fixture["dis"] = str(root / entry["dis"])
        fixtures.append(fixture)
    return fixtures


def vmaf_version(vmaf: Path) -> str:
    """`vmaf --version` first line, or `unknown`."""
    try:
        result = run_bounded([str(vmaf), "--version"], timeout_seconds=60, max_output_bytes=65536)
    except (TimeoutError, RuntimeError, ValueError):
        return "unknown"
    lines = (result.stdout or result.stderr).strip().splitlines()
    return lines[0] if lines else "unknown"


def image_block(root: Path, digest: str | None) -> dict[str, Any]:
    """Facts about the image, ending with the checks that tie the files to the build."""
    info = read_build_info(str(root / "image" / "build-info.json"))
    vmaf = root / "build" / "tools" / "vmaf"
    libs = sorted((root / "build" / "src").glob("libvmaf.so.*.*.*"))
    vmaf_hash = sha256_file(vmaf)
    lib_hash = sha256_file(libs[0]) if libs else None
    keys = ("source_commit", "source_ref", "recipe_commit", "built_by_workflow", "image_arch", "compiler",
            "libc", "base_image", "tag")  # fmt: skip
    block: dict[str, Any] = {"kind": info.get("kind", "container-image")}
    block.update({key: info.get(key, "unknown") for key in keys})
    block.update(
        {
            "digest": digest,
            "vmaf_version": vmaf_version(vmaf),
            "vmaf_sha256": vmaf_hash,
            "libvmaf_sha256": lib_hash,
            "files_match_build": vmaf_hash is not None
            and vmaf_hash == info.get("vmaf_sha256")
            and lib_hash == info.get("libvmaf_sha256"),
            "python": platform.python_version(),
        }
    )
    return block


def not_exercised(
    report: Mapping[str, Any], skipped: Sequence[str], not_applicable: Mapping[str, str]
) -> list[dict[str, str]]:
    """What this report does not cover, and why."""
    host = report["host"]
    on_mac = host["platform"] == "darwin"
    items = [("tiny-AI / ONNX Runtime", "built with -Denable_dnn=disabled")]
    if on_mac:
        items.append(("CUDA, SYCL and HIP twins", "not available on macOS"))
        if report["metal_equivalence"]["status"] == "no_device":
            items.append(("Metal twins", "this host exposes no usable Metal device"))
            items.append(("Metal parity gate", "this host exposes no usable Metal device"))
    else:
        items.append(("Metal", "needs macOS; this is a Linux container with no Metal device"))
        items.append(
            ("CUDA, SYCL and HIP twins", "the image holds no GPU SDK and a CPU-only build")
        )
    if "golden" not in not_applicable:
        items.append(
            ("slow-marked Python tests", "the golden gate runs -m 'not slow', as make does")
        )
    flags = host["dispatch_flags"]
    if host["machine"] in ("aarch64", "arm64"):
        if "sve2" not in flags:
            items.append(("SVE2 kernels", "this CPU does not report SVE2 (AT_HWCAP2 bit 1)"))
        items.append(("x86 kernels", "not applicable on arm64"))
    else:
        items.append(("NEON and SVE2 kernels", "not applicable on x86_64"))
        if "avx512" not in flags:
            items.append(("AVX-512 kernels", "this CPU does not report AVX-512"))
    items += [
        (f"{name} check", reason) for name, reason in not_applicable.items() if name != "metal"
    ]
    items += [(name, "skipped by a maintainer option") for name in skipped]
    return [{"item": item, "reason": reason} for item, reason in items]


PASSING = {
    "dispatch_equivalence": ("identical",),
    "reference_equivalence": ("identical",),
    "metal_equivalence": ("identical", "no_device", "not_applicable"),
    "metal_gate": ("pass", "no_device", "not_applicable"),
    "unit_tests": ("pass",),
    "golden_gate": ("pass", "not_applicable"),
}
CHECK_KEYS = {
    "dispatch": "dispatch_equivalence",
    "reference": "reference_equivalence",
    "metal": "metal_equivalence",
    "gate": "metal_gate",
    "unit": "unit_tests",
    "golden": "golden_gate",
}


def verdict_of(report: Mapping[str, Any], skipped: Sequence[str]) -> tuple[str, list[str]]:
    """`pass`, `fail` or `incomplete`, with the names of the failed checks."""
    failed = [
        key
        for check, key in CHECK_KEYS.items()
        if check not in skipped and report[key]["status"] not in PASSING[key]
    ]
    if not report["image"]["files_match_build"]:
        failed.append("image_files_match_build")
    if failed:
        return "fail", failed
    return ("incomplete" if skipped else "pass"), []


EMPTY_SUITE = {"status": "not_run", "total": 0, "passed": 0, "failed": 0, "skipped": 0,
               "failures": []}  # fmt: skip


def _equivalence_sections(
    args: argparse.Namespace,
    root: Path,
    host: Mapping[str, Any],
    skipped: Sequence[str],
    not_applicable: Mapping[str, str],
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Dispatch, reference and Metal equivalence (the last two reuse the dispatch
    scores), and the raw CPU and Metal scores the state-row map reads."""
    idle = {"status": "not_run"}
    metal_idle = {"status": "not_applicable"} if "metal" in not_applicable else idle
    sections: dict[str, Any] = {
        "dispatch_equivalence": dict(idle),
        "reference_equivalence": dict(idle),
        "metal_equivalence": dict(metal_idle),
    }
    raw_scores: dict[str, Any] = {"cpu": {}, "metal": {}}
    if "dispatch" in skipped:
        return sections, raw_scores
    vmaf = str(root / "build" / "tools" / "vmaf")
    fixtures = load_fixtures(root)
    sections["dispatch_equivalence"], raw = run_dispatch_equivalence(
        vmaf, fixtures, timeout_seconds=args.fixture_timeout
    )
    raw_scores["cpu"] = {fixture_id: pair[0] for fixture_id, pair in raw.items()}
    if "reference" not in skipped:
        ref_dir = Path(args.reference_dir or root / "reference")
        sections["reference_equivalence"] = run_reference_equivalence(
            raw, ref_dir, machine=host["machine"], host_flags=host["dispatch_flags"]
        )
    if "metal" not in skipped and "metal" not in not_applicable:
        sections["metal_equivalence"], raw_scores["metal"] = run_metal_equivalence_raw(
            vmaf, fixtures, raw_scores["cpu"], timeout_seconds=args.fixture_timeout
        )
    return sections, raw_scores


def _gate_section(
    root: Path, metal_status: str, skipped: Sequence[str], row_map: Mapping[str, Any] | None
) -> dict[str, Any]:
    """The parity gate's Metal cells on every fixture (ADR-1496)."""
    if "gate" in skipped:
        return {"status": "not_run"}
    return run_metal_gate(
        root,
        str(root / "build" / "tools" / "vmaf"),
        load_fixtures(root),
        row_map.get("gate") if row_map else None,
        metal_status=metal_status,
        timeout_seconds=GATE_TIMEOUT_SECONDS,
    )


def _suite_sections(
    root: Path, skipped: Sequence[str], not_applicable: Mapping[str, str]
) -> dict[str, Any]:
    """Unit executables and the Netflix golden gate."""
    unit = dict(EMPTY_SUITE)
    golden = dict(EMPTY_SUITE)
    if "golden" in not_applicable:
        golden = {**EMPTY_SUITE, "status": "not_applicable", "reason": not_applicable["golden"]}
    if "unit" not in skipped:
        unit = run_unit_tests(
            root / "image" / "unit-tests.json", timeout_seconds=UNIT_TIMEOUT_SECONDS
        )
    if "golden" not in skipped and "golden" not in not_applicable:
        golden = run_golden_gate(root, timeout_seconds=GOLDEN_TIMEOUT_SECONDS)
    return {"unit_tests": unit, "golden_gate": golden}


def applicability(root: Path, host: Mapping[str, Any]) -> dict[str, str]:
    """Checks that make no sense for this package or platform, with the reason."""
    info = read_build_info(str(root / "image" / "build-info.json"))
    found = {str(k): str(v) for k, v in info.get("not_applicable", {}).items()}
    if host["platform"] != "darwin":
        found["metal"] = "Metal exists only on macOS"
    return found


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    """Run the selected checks and assemble the report."""
    root = Path(args.image_root)
    skipped = [name for name in CHECKS if name in args.skip]
    if "dispatch" in skipped:  # reference and Metal compare the dispatch run's scores
        skipped += [name for name in ("reference", "metal") if name not in skipped]
    if "metal" in skipped and "gate" not in skipped:  # the gate runs where Metal ran
        skipped.append("gate")
    host = collect_host_facts()
    not_applicable = applicability(root, host)
    now = datetime.datetime.now(datetime.timezone.utc)
    report: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "tool": {"name": "vmaf-tester-report", "version": __version__},
        "generated_utc": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "image": image_block(root, args.image_digest),
        "host": host,
    }
    sections, raw_scores = _equivalence_sections(args, root, host, skipped, not_applicable)
    report.update(sections)
    row_map = load_row_map(root / "image" / "metal-rows.json")
    report["metal_gate"] = _gate_section(
        root, report["metal_equivalence"]["status"], skipped, row_map
    )
    report.update(_suite_sections(root, skipped, not_applicable))
    report["metal_rows"] = evaluate_rows(
        row_map, report["unit_tests"].get("cases", {}), raw_scores["cpu"], raw_scores["metal"],
        report["metal_gate"],
    )  # fmt: skip
    report["not_exercised"] = not_exercised(report, skipped, not_applicable)
    report["verdict"], report["failed_checks"] = verdict_of(report, skipped)
    report["note"] = args.note
    report["report_sha256"] = report_digest(report)
    return report


def _rows_line(rows: Mapping[str, Any]) -> str:
    counts = rows.get("counts")
    if not counts:
        return f"metal state rows: {rows['status']}"
    return (
        f"metal state rows: {counts['pass']} measured passing, {counts['fail']} failing, "
        f"{counts['not_measured']} not measured"
    )


def suggested_file_name(report: Mapping[str, Any]) -> str:
    """`docs/hardware-reports/<date>-<cpu-slug>.json` for this report."""
    slug = re.sub(r"[^a-z0-9]+", "-", report["host"]["cpu_model"].lower()).strip("-")[:60]
    return f"docs/hardware-reports/{report['generated_utc'][:10]}-{slug.strip('-') or 'cpu'}.json"


def summarize(report: Mapping[str, Any]) -> str:
    """Short human summary for stderr."""
    host = report["host"]
    lines = [
        f"vmaf-tester-report: verdict {report['verdict']}",
        f"cpu: {host['cpu_model']} ({host['machine']}); dispatch: {', '.join(host['dispatch_flags']) or 'none'}",
        f"dispatch equivalence: {report['dispatch_equivalence']['status']}",
        f"reference equivalence: {report['reference_equivalence']['status']}",
        f"metal equivalence: {report['metal_equivalence']['status']}",
        f"metal parity gate: {report['metal_gate']['status']}",
        _rows_line(report["metal_rows"]),
        summary_line("unit tests", report["unit_tests"]),
        summary_line("golden gate", report["golden_gate"]),
    ]
    if report["failed_checks"]:
        lines.append("failed: " + ", ".join(report["failed_checks"]))
    lines.append("file name for a pull request: " + suggested_file_name(report))
    lines.append("not exercised: " + "; ".join(item["item"] for item in report["not_exercised"]))
    return "\n".join(lines)


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="vmaf-tester-report", description=__doc__)
    parser.add_argument("--image-root", default=os.environ.get("VMAFX_IMAGE_ROOT", DEFAULT_ROOT))
    parser.add_argument("--reference-dir", default=None, help="reference files (default: baked)")
    parser.add_argument("--image-digest", default=None, help="sha256:... of the pulled image")
    parser.add_argument("--note", default="", help="free text, at most 500 characters")
    parser.add_argument("--fixture-timeout", type=float, default=FIXTURE_TIMEOUT_SECONDS)
    parser.add_argument(
        "--skip", action="append", default=[], choices=CHECKS,
        help="maintainer option: skip a check; the verdict becomes 'incomplete'",
    )  # fmt: skip
    sub = parser.add_subparsers(dest="command")
    gen = sub.add_parser("generate-reference", help="image build step: write reference files")
    gen.add_argument("out_dir")
    return parser


def generate(args: argparse.Namespace) -> int:
    """Build-time entry: write the reference files for this architecture."""
    root = Path(args.image_root)
    host = collect_host_facts()
    paths = generate_reference(
        str(root / "build" / "tools" / "vmaf"),
        load_fixtures(root),
        Path(args.out_dir),
        machine=host["machine"],
        flags=host["dispatch_flags"],
        timeout_seconds=args.fixture_timeout,
    )
    print("\n".join(str(path) for path in paths), file=sys.stderr)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    args.note = args.note[:500]
    if args.command == "generate-reference":
        return generate(args)
    report = build_report(args)
    print(json.dumps(report, indent=2, ensure_ascii=True))
    print(summarize(report), file=sys.stderr)
    return {"pass": 0, "fail": 1}.get(report["verdict"], 2)
