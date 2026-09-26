# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""CLI for the VMAFx RC1 external tester evidence bundle."""

from __future__ import annotations

import argparse
import json
from collections.abc import Callable, Sequence

from vmaf_rc1_tester.bundle import create_report_bundle
from vmaf_rc1_tester.probe import ALL_SUPPORTED_BACKENDS, DiagnosticReport, run_full_probe
from vmaf_rc1_tester.tools_catalog import render_tools_markdown
from vmaf_rc1_tester.validate import ValidationResult, run_smoke_validation

EXIT_FAILURE = 1
EXIT_INCOMPLETE = 2
EXIT_BACKEND_UNAVAILABLE = 100


def _non_negative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be a non-negative integer")
    return parsed


def _add_binary_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--vmaf-bin", help="Path to an executable vmaf binary")
    parser.add_argument("--build-dir", help="Meson build directory containing tools/vmaf")


def _add_backend_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--backend",
        action="append",
        choices=ALL_SUPPORTED_BACKENDS,
        required=True,
        help="Explicit backend to test; repeat for more than one backend",
    )
    parser.add_argument(
        "--device-index",
        type=_non_negative_int,
        default=0,
        help=(
            "Runtime-visible accelerator ordinal (default: 0); applies to every "
            "requested accelerator backend"
        ),
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="vmaf-rc1-report",
        description="Collect bounded RC1 hardware and explicit-backend validation evidence.",
    )
    subparsers = parser.add_subparsers(dest="subcommand", required=True)
    probe = subparsers.add_parser("probe", help="Discover host hardware and toolchain visibility")
    _add_binary_arguments(probe)
    probe.add_argument("--json", action="store_true", help="Emit machine-readable diagnostics")
    probe.set_defaults(handler=_handle_probe)
    validate = subparsers.add_parser("validate", help="Run four-frame explicit-backend checks")
    _add_binary_arguments(validate)
    _add_backend_arguments(validate)
    validate.set_defaults(handler=_handle_validate)
    tools = subparsers.add_parser("list-tools", help="Show RC1, RC2, and RC3 tool boundaries")
    tools.set_defaults(handler=_handle_list_tools)
    bundle = subparsers.add_parser("bundle", help="Create one shareable evidence archive")
    _add_binary_arguments(bundle)
    _add_backend_arguments(bundle)
    bundle.add_argument("--out-dir", default=".", help="Destination directory")
    bundle.add_argument("--format", choices=("tar.gz", "zip"), default="tar.gz")
    bundle.set_defaults(handler=_handle_bundle)
    return parser


def _probe(args: argparse.Namespace) -> DiagnosticReport:
    return run_full_probe(vmaf_path=args.vmaf_bin, build_dir=args.build_dir)


def _handle_probe(args: argparse.Namespace) -> int:
    report = _probe(args)
    if args.json:
        print(json.dumps(report.to_dict(), indent=2))
        return 0
    print("=== VMAFx RC1 hardware and toolchain probe ===")
    print(f"Platform: {report.os.distro} ({report.os.machine})")
    print(f"CPU: {report.cpu.model} ({report.cpu.logical_cores} logical cores)")
    print(
        f"SIMD: AVX2={report.cpu.has_avx2} AVX-512={report.cpu.has_avx512} "
        f"NEON={report.cpu.has_neon}"
    )
    devices = report.accelerators.all_devices()
    print(
        "Accelerators: "
        + (
            ", ".join(
                f"{item.backend}[{item.ordinal if item.ordinal is not None else '?'}]:{item.name}"
                for item in devices
            )
            or "none"
        )
    )
    print(f"vmaf: {report.vmaf_binary.path or 'not found'}")
    if report.vmaf_binary.exists:
        print(f"Version: {report.vmaf_binary.version}")
        selectors = ", ".join(report.vmaf_binary.accepted_backend_selectors) or "none discovered"
        print(f"Accepted selectors: {selectors}")
    return 0


def _run_validations(
    report: DiagnosticReport,
    backends: list[str],
    device_index: int,
) -> list[ValidationResult]:
    requested = list(dict.fromkeys(backends))
    cpu_requested = "cpu" in requested
    cpu_result = run_smoke_validation(
        report,
        backend="cpu",
        device_index=device_index,
        requested_by_user=cpu_requested,
    )
    results = [cpu_result]
    for backend in requested:
        if backend == "cpu":
            continue
        results.append(
            run_smoke_validation(
                report,
                backend=backend,
                device_index=device_index,
                cpu_reference=cpu_result,
                requested_by_user=True,
            )
        )
    return results


def _result_exit_code(results: list[ValidationResult]) -> int:
    verdicts = {result.verdict for result in results}
    known_verdicts = {"PASS", "FAIL", "SKIPPED", "BACKEND_UNAVAILABLE"}
    if not verdicts or verdicts - known_verdicts:
        return EXIT_FAILURE
    if "FAIL" in verdicts:
        return EXIT_FAILURE
    if "BACKEND_UNAVAILABLE" in verdicts:
        return EXIT_BACKEND_UNAVAILABLE
    if "SKIPPED" in verdicts:
        return EXIT_INCOMPLETE
    return 0 if verdicts == {"PASS"} else EXIT_FAILURE


def _print_results(results: list[ValidationResult]) -> None:
    for result in results:
        observed = result.backend_observed or "not reported"
        implicit = " (automatic reference)" if not result.requested_by_user else ""
        print(f"{result.backend_requested}{implicit}: {result.verdict}")
        print(f"  observed backend: {observed}")
        if result.device_index is not None:
            print(f"  device index: {result.device_index}")
        print(f"  process exit: {result.process_exit_code}")
        print(f"  duration: {result.duration_seconds}s")
        if result.reference_backend:
            print(f"  correctness reference: {result.reference_backend}")
        if result.max_abs_score_delta is not None:
            print(f"  maximum metric delta: {result.max_abs_score_delta:.9g}")
        print(f"  notes: {result.notes}")


def _handle_validate(args: argparse.Namespace) -> int:
    results = _run_validations(_probe(args), args.backend, args.device_index)
    _print_results(results)
    return _result_exit_code(results)


def _handle_list_tools(_args: argparse.Namespace) -> int:
    print(render_tools_markdown())
    return 0


def _handle_bundle(args: argparse.Namespace) -> int:
    report = _probe(args)
    results = _run_validations(report, args.backend, args.device_index)
    artifact = create_report_bundle(
        report=report,
        validations=results,
        dest_dir=args.out_dir,
        archive_format=args.format,
    )
    print("=== VMAFx RC1 tester report bundle created ===")
    print(f"Archive: {artifact.archive_path}")
    print(f"SHA-256: {artifact.archive_sha256}")
    print(f"Report: {artifact.report_path}")
    _print_results(results)
    print("Share the archive with the maintainer; inspect report.md first for private data.")
    return _result_exit_code(results)


def main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    handler: Callable[[argparse.Namespace], int] = args.handler
    return handler(args)
