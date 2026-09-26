# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Create privacy-redacted, integrity-checkable RC1 tester archives."""

from __future__ import annotations

import datetime
import gzip
import hashlib
import io
import json
import math
import re
import tarfile
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from vmaf_rc1_tester import __version__
from vmaf_rc1_tester.probe import REPO_ROOT, DiagnosticReport, SubprocessRunner, _run_cmd
from vmaf_rc1_tester.tools_catalog import get_tools_catalog, render_tools_markdown
from vmaf_rc1_tester.validate import ValidationResult


@dataclass(frozen=True)
class BundleArtifact:
    archive_path: Path
    archive_sha256: str
    staging_dir: Path
    manifest_path: Path
    report_path: Path


def _calc_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(65536):
            digest.update(chunk)
    return digest.hexdigest()


def _collector_checkout_revision(runner: SubprocessRunner | None = None) -> str:
    rc, out, _ = _run_cmd(["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"], runner=runner)
    return out if rc == 0 and out else "unknown"


def _redact_text(value: str) -> str:
    replacements = sorted(
        ((str(REPO_ROOT), "$REPO"), (str(Path.home()), "$HOME")),
        key=lambda item: len(item[0]),
        reverse=True,
    )
    for original, replacement in replacements:
        if original and original != "/":
            value = value.replace(original, replacement)
    temp_prefix = re.escape(str(Path(tempfile.gettempdir()) / "vmafx_rc1_"))
    return re.sub(rf"{temp_prefix}[^/\s]+", "$REPORT_TMP", value)


def _redact(value: Any) -> Any:
    if isinstance(value, str):
        return _redact_text(value)
    if isinstance(value, list):
        return [_redact(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _redact(item) for key, item in value.items()}
    if isinstance(value, float) and not math.isfinite(value):
        return None
    return value


def _dumps_json(value: Any) -> str:
    return json.dumps(_redact(value), indent=2, allow_nan=False) + "\n"


def _md_cell(value: object) -> str:
    return _redact_text(str(value)).replace("|", "\\|").replace("\n", "<br>")


def _system_section(report: DiagnosticReport) -> list[str]:
    devices = report.accelerators.all_devices()
    device_lines = [
        f"- **{device.backend.upper()}"
        + (f" device {device.ordinal}" if device.ordinal is not None else "")
        + f"**: `{_md_cell(device.name)}`"
        + (f" (driver: `{_md_cell(device.driver_version)}`)" if device.driver_version else "")
        for device in devices
    ] or ["- No GPU accelerator was detected by the bounded probes."]
    return [
        "## System and hardware",
        "",
        "| Attribute | Value |",
        "| :--- | :--- |",
        f"| OS | {_md_cell(report.os.distro)} |",
        f"| Kernel | {_md_cell(report.os.release)} |",
        f"| Architecture | {_md_cell(report.os.machine)} |",
        f"| Container | {report.os.is_container} |",
        f"| CPU | {_md_cell(report.cpu.model)} |",
        f"| Logical cores | {report.cpu.logical_cores} |",
        f"| AVX2 / AVX-512 / NEON | {report.cpu.has_avx2} / {report.cpu.has_avx512} / {report.cpu.has_neon} |",
        "",
        "### Detected accelerators",
        "",
        *device_lines,
    ]


def _toolchain_section(report: DiagnosticReport) -> list[str]:
    toolchain = report.toolchain
    rows = (
        ("C compiler", toolchain.c_compiler),
        ("C++ compiler", toolchain.cxx_compiler),
        ("Meson", toolchain.meson),
        ("Ninja", toolchain.ninja),
        ("NVCC", toolchain.nvcc),
        ("ICPX", toolchain.icpx),
        ("HIPCC", toolchain.hipcc),
        ("Metal compiler", toolchain.metal_compiler),
    )
    return [
        "## Toolchain",
        "",
        "| Tool | Version or status |",
        "| :--- | :--- |",
        *(f"| {name} | `{_md_cell(value)}` |" for name, value in rows),
    ]


def _binary_section(report: DiagnosticReport) -> list[str]:
    binary = report.vmaf_binary
    selectors = ", ".join(binary.accepted_backend_selectors) or "none discovered"
    return [
        "## VMAFx binary",
        "",
        "| Property | Value |",
        "| :--- | :--- |",
        f"| Path | `{_md_cell(binary.path or 'not found')}` |",
        f"| Executable found | {binary.exists} |",
        f"| Version | `{_md_cell(binary.version or 'unknown')}` |",
        f"| SHA-256 | `{binary.sha256 or 'unavailable'}` |",
        f"| Accepted backend selectors | `{selectors}` |",
        "",
        (
            "> Selector names come from `vmaf --help`; they do not prove a backend was compiled "
            "or ran. The explicit smoke result below supplies bounded backend-state and "
            "emitted-metric correctness evidence."
        ),
    ]


def _validation_section(result: ValidationResult) -> list[str]:
    command = json.dumps(_redact(result.command_argv), ensure_ascii=False)
    log = _redact_text(result.stdout or result.stderr or "(no process output)")
    log_lines = [f"    {line}" for line in log.splitlines()] or ["    (no process output)"]
    return [
        f"### `{result.backend_requested}`: {result.verdict}",
        "",
        f"- Requested by tester: `{result.requested_by_user}`",
        f"- Observed backend: `{result.backend_observed or 'not reported'}`",
        f"- Device index: `{result.device_index if result.device_index is not None else 'not applicable'}`",
        f"- Environment overrides: `{_md_cell(json.dumps(result.environment_overrides, sort_keys=True))}`",
        f"- Process exit code: `{result.process_exit_code}`",
        f"- Duration: `{result.duration_seconds}s`",
        f"- Correctness reference: `{result.reference_backend or 'unavailable'}`",
        f"- Parity tolerance: `{result.parity_tolerance if result.parity_tolerance is not None else 'unavailable'}`",
        f"- Maximum absolute metric delta: `{result.max_abs_score_delta if result.max_abs_score_delta is not None else 'unavailable'}`",
        f"- Model SHA-256: `{result.model_sha256 or 'unavailable'}`",
        f"- Notes: {_md_cell(result.notes)}",
        f"- Command argv: `{_md_cell(command)}`",
        "",
        "Process output:",
        "",
        *log_lines,
        "",
    ]


def _render_report(
    report: DiagnosticReport,
    validations: list[ValidationResult],
    revision: str,
    created_at: str,
) -> str:
    verdicts = ", ".join(f"{item.backend_requested}={item.verdict}" for item in validations)
    lines = [
        "# VMAFx RC1 external tester report",
        "",
        f"- Created (UTC): `{created_at}`",
        f"- Collector checkout revision: `{revision}` (binary identity is the version and SHA-256 below)",
        f"- Validation summary: `{verdicts}`",
        "",
        *_system_section(report),
        "",
        *_toolchain_section(report),
        "",
        *_binary_section(report),
        "",
        "## Explicit backend correctness smoke validation",
        "",
        (
            "> PASS means the requested backend state initialized and four frames of emitted "
            "model metrics matched the pinned CPU result or this run's automatic CPU reference "
            "within 5e-5: the ADR-0214 bound for listed feature metrics, conservatively adopted "
            "by ADR-1342 for overall VMAF too. Frame 3 exercises temporal motion. `backend_used` "
            "remains backend-state evidence, not proof that every individual feature ran on the "
            "accelerator; this smoke does not replace the compiled backend suites or RC2 "
            "benchmarking."
        ),
        "",
    ]
    for validation in validations:
        lines.extend(_validation_section(validation))
    lines.extend([render_tools_markdown(), ""])
    return _redact_text("\n".join(lines))


def _write_validation_files(staging: Path, validations: list[ValidationResult]) -> None:
    for result in validations:
        prefix = f"smoke_{result.backend_requested}"
        log = f"=== STDOUT ===\n{result.stdout}\n\n=== STDERR ===\n{result.stderr}\n"
        (staging / f"{prefix}.log").write_text(_redact_text(log), encoding="utf-8")
        if result.json_output is not None:
            (staging / f"{prefix}.json").write_text(
                _dumps_json(result.json_output),
                encoding="utf-8",
            )


def _write_checksums(staging: Path) -> dict[str, dict[str, Any]]:
    files: dict[str, dict[str, Any]] = {}
    lines: list[str] = []
    for path in sorted(staging.iterdir()):
        if path.is_file() and path.name not in ("manifest.json", "SHA256SUMS"):
            digest = _calc_sha256(path)
            files[path.name] = {"sha256": digest, "size_bytes": path.stat().st_size}
            lines.append(f"{digest}  {path.name}")
    sums = staging / "SHA256SUMS"
    sums.write_text("\n".join(lines) + "\n", encoding="utf-8")
    files[sums.name] = {"sha256": _calc_sha256(sums), "size_bytes": sums.stat().st_size}
    return files


def _write_bundle_files(
    staging: Path,
    report: DiagnosticReport,
    validations: list[ValidationResult],
    revision: str,
    created_at: str,
) -> tuple[Path, Path]:
    staging.mkdir(parents=True, exist_ok=False)
    report_path = staging / "report.md"
    report_path.write_text(
        _render_report(report, validations, revision, created_at), encoding="utf-8"
    )
    diagnostics = {
        "diagnostics": report.to_dict(),
        "validations": [result.to_dict() for result in validations],
        "tools_catalog": get_tools_catalog(),
    }
    (staging / "diagnostics.json").write_text(_dumps_json(diagnostics), encoding="utf-8")
    _write_validation_files(staging, validations)
    file_hashes = _write_checksums(staging)
    manifest = {
        "schema_version": "1.0",
        "tool": "vmaf-rc1-report",
        "tool_version": __version__,
        "created_at": created_at,
        "collector_checkout_revision": revision,
        "platform": f"{report.os.system.lower()}-{report.os.machine.lower()}",
        "validation_verdicts": {item.backend_requested: item.verdict for item in validations},
        "files": file_hashes,
    }
    manifest_path = staging / "manifest.json"
    manifest_path.write_text(_dumps_json(manifest), encoding="utf-8")
    return report_path, manifest_path


def _archive_zip(staging: Path, archive_path: Path) -> None:
    with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as target:
        for path in sorted(staging.iterdir()):
            member = zipfile.ZipInfo(f"{staging.name}/{path.name}", (1980, 1, 1, 0, 0, 0))
            member.create_system = 3
            member.compress_type = zipfile.ZIP_DEFLATED
            member.external_attr = (0o100644 & 0xFFFF) << 16
            target.writestr(member, path.read_bytes())


def _archive_tar(staging: Path, archive_path: Path) -> None:
    with (
        archive_path.open("wb") as raw_stream,
        gzip.GzipFile(filename="", mode="wb", fileobj=raw_stream, mtime=0) as compressed,
        tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as target,
    ):
        for path in sorted(staging.iterdir()):
            payload = path.read_bytes()
            member = tarfile.TarInfo(f"{staging.name}/{path.name}")
            member.size = len(payload)
            member.mode = 0o644
            member.mtime = 0
            member.uid = 0
            member.gid = 0
            member.uname = ""
            member.gname = ""
            target.addfile(member, io.BytesIO(payload))


def _archive(staging: Path, archive_format: str) -> Path:
    suffix = ".zip" if archive_format == "zip" else ".tar.gz"
    archive_path = staging.parent / f"{staging.name}{suffix}"
    if archive_format == "zip":
        _archive_zip(staging, archive_path)
    else:
        _archive_tar(staging, archive_path)
    return archive_path


def create_report_bundle(
    report: DiagnosticReport,
    validations: list[ValidationResult],
    dest_dir: Path | str = ".",
    archive_format: str = "tar.gz",
    runner: SubprocessRunner | None = None,
) -> BundleArtifact:
    """Write report evidence and one shareable archive."""
    if not validations:
        raise ValueError("at least one explicit backend validation is required")
    destination = Path(dest_dir).expanduser().resolve()
    destination.mkdir(parents=True, exist_ok=True)
    now = datetime.datetime.now(datetime.timezone.utc)
    created_at = now.isoformat(timespec="seconds")
    timestamp = now.strftime("%Y%m%dT%H%M%S%fZ")
    platform_slug = re.sub(r"[^a-z0-9_-]+", "-", f"{report.os.system}-{report.os.machine}".lower())
    staging = destination / f"vmafx-rc1-report-{platform_slug}-{timestamp}"
    revision = _collector_checkout_revision(runner=runner)
    report_path, manifest_path = _write_bundle_files(
        staging, report, validations, revision, created_at
    )
    archive_path = _archive(staging, archive_format)
    return BundleArtifact(
        archive_path=archive_path,
        archive_sha256=_calc_sha256(archive_path),
        staging_dir=staging,
        manifest_path=manifest_path,
        report_path=report_path,
    )
