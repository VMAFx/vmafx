#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Find actionable modernization gaps across repo code, docs, and local state.

The audit is intentionally read-only. It scans curated source/doc roots plus
the local gitignored state files and ranks markers such as stubs, scaffolds,
deferred work, placeholders, and implementation TODOs into a human-readable
queue. It does not edit BACKLOG.md, open PRs, or run external tools.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence

REPO_ROOT = Path(__file__).resolve().parents[2]

DEFAULT_SCAN_ROOTS: tuple[str, ...] = (
    ".github/workflows",
    "ai",
    "docs/ai",
    "docs/api",
    "docs/backends",
    "docs/development",
    "docs/mcp",
    "docs/metrics",
    "docs/usage",
    "core/src",
    "core/tools",
    "mcp-server",
    "scripts",
    "tools",
)

DEFAULT_STATE_FILES: tuple[str, ...] = (
    ".workingdir2/OPEN.md",
    ".workingdir2/BACKLOG.md",
    "docs/state.md",
)

TEXT_SUFFIXES: frozenset[str] = frozenset(
    {
        ".c",
        ".cc",
        ".cpp",
        ".cu",
        ".h",
        ".hpp",
        ".hip",
        ".md",
        ".py",
        ".sh",
        ".toml",
        ".yaml",
        ".yml",
    }
)

SCRIPT_CLUSTER_MIN_COUNT = 6
SKIP_FILE_NAMES: frozenset[str] = frozenset({"AGENTS.md", "project_modernization_audit.py"})

SKIP_PARTS: frozenset[str] = frozenset(
    {
        ".git",
        ".mypy_cache",
        ".pytest_cache",
        ".ruff_cache",
        ".venv",
        "__pycache__",
        "archive",
        "build",
        "build-docs",
        "node_modules",
        "site",
    }
)

# Paths where "placeholder" is calibration-framework domain vocabulary (a
# documented STATUS value: "calibrated" | "placeholder"), not a code-gap
# marker.  False-positive category: developer-tools placeholder noise (~30
# findings prior to this exclude rule).
CALIBRATION_PLACEHOLDER_PATHS: frozenset[str] = frozenset(
    {
        "scripts/ci/cross_backend_calibration.py",
        "scripts/ci/cross_backend_parity_gate.py",
        "scripts/ci/gpu_ulp_calibration.yaml",
    }
)

# H2 headings in state files that denote resolved rows.  The scanner skips
# all T-* matches that fall under one of these headings (until the next H2).
# False-positive category: state.md / BACKLOG / OPEN closed-row noise (~70
# findings prior to this exclude rule).
CLOSED_SECTION_HEADINGS_RE = re.compile(
    r"^##\s+(Recently closed|Resolved|Confirmed not-affected)",
    re.IGNORECASE,
)

# Line-level marker for explicitly closed rows.  Two forms:
#   1. A "(YYYY-MM-DD)" date stamp at end of line (possibly followed by
#      whitespace or a Markdown table-cell "|" before the real EOL) — used
#      by docs/state.md table rows in the "Recently closed" section that have
#      been moved but still appear in the Open section during authoring.
#   2. A literal "closed: " prefix — used in BACKLOG/OPEN inline-close rows.
CLOSED_ROW_RE = re.compile(
    r"\bclosed:\s|\(\d{4}-\d{2}-\d{2}\)\s*\|?\s*$",
    re.IGNORECASE,
)

BLOCKED_RE = re.compile(
    r"\b(blocked|waiting on|external trigger|manual access|redistribution|"
    r"legal|licen[cs]e|upstream|netflix releases|stability window|"
    r"no discrete|hardware unavailable|gated|model weights|dataset gated)\b",
    re.IGNORECASE,
)

HISTORICAL_CONTEXT_RE = re.compile(
    r"\b("
    r"already|closed|fixed|former(?:ly)?|historical|landed|legacy|"
    r"no longer|old|pre-fix|previous(?:ly)?|prior|replace[ds]?|"
    r"retired|rewritten|superseded"
    r")\b",
    re.IGNORECASE,
)

ENOSYS_CONTRACT_RE = re.compile(
    r"\b("
    r"built without|compiled out|contract|degrade gracefully|disabled-build|"
    r"omitted|optional|pins? the -ENOSYS|stub-only|vmaf_[a-z0-9_]+_available|"
    r"HAVE_[A-Z0-9_]+|enable_[a-z0-9_]+=(?:false|disabled)|"
    r"scaffold posture|runtime not ready|non-ROCm builds|CPU-flagged|"
    r"buffer-type plumbing|volkInitialize|load_volk_once|"
    r"without (?:hipcc|[a-z0-9_ -]+loader|[a-z0-9_ -]+runtime)|"
    r"no [a-z0-9_ -]+loader|symbol is not available|"
    r"lifecycle helper returns -ENOSYS|every lifecycle helper returns -ENOSYS|"
    r"init\(\) returns -ENOSYS|returns -ENOSYS until|falling through to the CPU"
    r")\b",
    re.IGNORECASE,
)

ERROR_TRANSLATION_CONTEXT_RE = re.compile(
    r"\b("
    r"case\s+[A-Za-z0-9_]*(?:NotSupported|Unsupported)|"
    r"(?:rc|err|error)\s+to\s+(?:a\s+)?negative\s+(?:POSIX\s+)?errno|"
    r"translate\s+(?:a\s+)?[A-Za-z0-9_ -]+error\s+(?:code\s+)?to\s+"
    r"(?:a\s+)?negative\s+(?:POSIX\s+)?errno"
    r")\b",
    re.IGNORECASE,
)

TEST_DOUBLE_CONTEXT_RE = re.compile(
    r"\b("
    r"tests? (?:can |keep |may )?(?:inject|substitute|stub|stubbed)|"
    r"unit tests? (?:inject|substitute|stub|stubbed)|"
    r"stub(?:bed)? (?:external binaries|session|runner|subprocess|responses?)|"
    r"fake [a-z0-9_-]+|runner argument lets unit tests"
    r")\b",
    re.IGNORECASE,
)

NON_IMPLEMENTATION_STUB_CONTEXT_RE = re.compile(
    r"\b("
    r"driver stub|stub signature must match|stub signatures must match|"
    r"type stubs?|pandas-stubs"
    r")\b",
    re.IGNORECASE,
)

ADR_STUB_CONTEXT_RE = re.compile(
    r"("
    r"\.md\.stub|docs/adr/.+\.stub|ADR number|ADR allocator|"
    r"stub file|reservation|--claim"
    r")",
    re.IGNORECASE,
)

MARKERS: tuple[tuple[str, re.Pattern[str], int], ...] = (
    ("not_implemented", re.compile(r"\b(NotImplementedError|NotImplemented)\b"), 95),
    ("enosys", re.compile(r"\bENOSYS\b|-ENOSYS"), 90),
    ("stub", re.compile(r"\b(stub|stubs)\b", re.IGNORECASE), 80),
    ("scaffold", re.compile(r"\b(scaffold|scaffold-only|scaffolding)\b", re.IGNORECASE), 78),
    ("synthetic_stub", re.compile(r"\bsynthetic-stub\b", re.IGNORECASE), 76),
    ("placeholder", re.compile(r"\bplaceholder\b", re.IGNORECASE), 72),
    ("todo", re.compile(r"\b(TODO|FIXME|XXX)\b"), 68),
    ("deferred", re.compile(r"\bdeferred\b", re.IGNORECASE), 60),
    ("blocked", re.compile(r"\bblocked\b", re.IGNORECASE), 58),
    ("limitation", re.compile(r"\b(limitations?|known limitations?)\b", re.IGNORECASE), 45),
    (
        "future_work",
        re.compile(r"\b(future work|follow-up|not yet implemented)\b", re.IGNORECASE),
        44,
    ),
)

AREA_RULES: tuple[tuple[str, str], ...] = (
    ("tools/vmaf-tune", "vmaf-tune"),
    ("tools/external-bench", "external-bench"),
    ("tools/vmaf-roi", "roi-tools"),
    ("ai", "tiny-ai"),
    ("model", "models"),
    ("mcp-server", "mcp"),
    ("core/src/feature", "feature-extractors"),
    ("core/src/cuda", "cuda"),
    ("core/src/sycl", "sycl"),
    ("core/src/vulkan", "vulkan"),
    ("core/src/hip", "hip"),
    ("core/src/dnn", "dnn"),
    ("libvmaf", "libvmaf"),
    (".github", "ci"),
    ("scripts", "developer-tools"),
    ("docs", "docs"),
)


@dataclass(frozen=True)
class Finding:
    id: str
    kind: str
    severity: int
    area: str
    path: str
    line: int | None
    title: str
    evidence: str
    action: str
    blocked: bool
    blocked_reason: str


@dataclass(frozen=True)
class AuditReport:
    repo_root: str
    findings: list[Finding]
    clusters: list[Finding]

    def to_json(self) -> dict:
        return {
            "repo_root": self.repo_root,
            "summary": summarize_findings(self.findings + self.clusters),
            "findings": [asdict(finding) for finding in self.findings],
            "clusters": [asdict(finding) for finding in self.clusters],
        }


def _relative(repo_root: Path, path: Path) -> str:
    try:
        return str(path.relative_to(repo_root))
    except ValueError:
        return str(path)


def _finding_id(kind: str, path: str, line: int | None, evidence: str) -> str:
    source = f"{kind}:{path}:{line}:{evidence}".encode("utf-8")
    return hashlib.blake2s(source, digest_size=8).hexdigest()[:12]


def _area_for(path: str) -> str:
    for prefix, area in AREA_RULES:
        if path == prefix or path.startswith(f"{prefix}/"):
            return area
    return "repo"


def _action_for(kind: str, path: str) -> str:
    action = "Review and classify into code work, docs correction, or explicit blocked debt."
    if kind in {"not_implemented", "enosys"}:
        action = "Implement the missing path or document the disabled-build contract if it is intentional."
    elif kind in {"stub", "scaffold", "synthetic_stub", "placeholder"}:
        if path.startswith("model/") or "/models/" in path:
            action = "Promote with real evidence or quarantine as smoke-only model-artifact debt."
        else:
            action = "Replace the placeholder with production code, or move the limitation to a blocked row."
    elif kind == "todo":
        action = (
            "Convert the inline TODO into a tracked backlog row or close it with a regression test."
        )
    elif kind in {"deferred", "blocked", "future_work"}:
        action = "Revalidate the trigger; keep blocked only when the dependency is still real."
    elif kind == "limitation":
        action = "Turn the limitation into a named testable backlog item when it affects user-visible behaviour."
    return action


def _blocked_reason(line: str) -> tuple[bool, str]:
    match = BLOCKED_RE.search(line)
    if match is None:
        return False, ""
    return True, match.group(0)


def _python_not_implemented_is_actionable(line: str) -> bool:
    """Return whether a Python ``NotImplementedError`` mention is live debt."""
    stripped = line.strip()
    lowered = stripped.lower()
    if lowered.startswith("except notimplementederror"):
        return False
    if re.match(r"^class\s+\w+\s*\(\s*NotImplementedError\s*\)\s*:", stripped):
        return False
    return bool(re.search(r"\braise\s+NotImplementedError\b", stripped))


def _enosys_is_contract(path: str, context: str) -> bool:
    if Path(path).suffix == ".md":
        return True
    if path.startswith("core/src/dnn/"):
        return True
    return bool(ENOSYS_CONTRACT_RE.search(context) or ERROR_TRANSLATION_CONTEXT_RE.search(context))


def _marker_suppressed(kind: str, path: str, line: str, context: str) -> bool:
    """Filter historical prose and non-debt exception handling.

    The audit should point at implementation gaps, not at docs explaining that a
    gap used to exist. Keep source-code ``raise NotImplementedError`` rows, but
    drop docstrings such as "replaces the NotImplementedError scaffold" and
    catch/exception-class lines that are part of normal error handling.

    Also suppresses ``placeholder`` matches in the calibration-framework scripts
    where ``placeholder`` is domain vocabulary for a STATUS value, not a gap
    marker (CALIBRATION_PLACEHOLDER_PATHS exclude rule).
    """
    # Cross-cutting early exits: historical prose and calibration-path vocabulary.
    # Developer-tools placeholder noise: "placeholder" in calibration scripts is
    # a documented STATUS enum value ("calibrated" | "placeholder"), not a code
    # gap.  Suppress only the placeholder marker; other markers still fire.
    if HISTORICAL_CONTEXT_RE.search(line) or (
        kind == "placeholder" and path in CALIBRATION_PLACEHOLDER_PATHS
    ):
        return True
    if kind == "not_implemented":
        if Path(path).suffix != ".py":
            return True
        return not _python_not_implemented_is_actionable(line)
    if kind == "enosys":
        return _enosys_is_contract(path, context)
    if kind in {"stub", "scaffold"}:
        return bool(
            ENOSYS_CONTRACT_RE.search(context)
            or TEST_DOUBLE_CONTEXT_RE.search(context)
            or NON_IMPLEMENTATION_STUB_CONTEXT_RE.search(context)
            or ADR_STUB_CONTEXT_RE.search(context)
        )
    return False


def _interesting_file(path: Path) -> bool:
    return path.suffix in TEXT_SUFFIXES and path.is_file()


def _skip_path(path: Path, *, include_archives: bool) -> bool:
    parts = set(path.parts)
    if include_archives:
        parts.discard("archive")
    if parts & SKIP_PARTS:
        return True
    if path.name in SKIP_FILE_NAMES:
        return True
    return path.name.startswith("test_") or path.name.startswith("test-") or "tests" in parts


def _iter_files(repo_root: Path, roots: Sequence[str], *, include_archives: bool) -> Iterable[Path]:
    for root_name in roots:
        root = repo_root / root_name
        if root.is_file() and _interesting_file(root):
            yield root
            continue
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if _skip_path(path.relative_to(repo_root), include_archives=include_archives):
                continue
            if _interesting_file(path):
                yield path


def scan_marker_findings(
    repo_root: Path,
    roots: Sequence[str],
    *,
    include_archives: bool = False,
    max_per_file: int = 5,
) -> list[Finding]:
    findings: list[Finding] = []
    for path in _iter_files(repo_root, roots, include_archives=include_archives):
        rel = _relative(repo_root, path)
        seen_in_file = 0
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            continue
        for line_no, line in enumerate(lines, start=1):
            context = "\n".join(lines[max(0, line_no - 8) : min(len(lines), line_no + 7)])
            for kind, pattern, base_severity in MARKERS:
                if pattern.search(line) is None:
                    continue
                if _marker_suppressed(kind, rel, line, context):
                    continue
                blocked, reason = _blocked_reason(line)
                severity = base_severity - (20 if blocked else 0)
                stripped = " ".join(line.strip().split())
                findings.append(
                    Finding(
                        id=_finding_id(kind, rel, line_no, stripped),
                        kind=kind,
                        severity=max(1, severity),
                        area=_area_for(rel),
                        path=rel,
                        line=line_no,
                        title=f"{kind.replace('_', ' ')} marker in {rel}",
                        evidence=stripped[:240],
                        action=_action_for(kind, rel),
                        blocked=blocked,
                        blocked_reason=reason,
                    )
                )
                seen_in_file += 1
                break
            if seen_in_file >= max_per_file:
                break
    return findings


def scan_state_files(repo_root: Path, state_files: Sequence[str]) -> list[Finding]:
    findings: list[Finding] = []
    row_re = re.compile(r"\bT-[A-Z0-9][A-Z0-9_-]*|\bT[0-9][A-Z0-9_-]*")
    h2_re = re.compile(r"^##\s+")
    for state_name in state_files:
        path = repo_root / state_name
        if not path.is_file():
            continue
        rel = _relative(repo_root, path)
        # Track whether the current H2 section is a closed / resolved section.
        # False-positive category: state.md closed-row noise (~70 findings).
        # Rows under "Recently closed", "Resolved", or "Confirmed not-affected"
        # headings are historical audit records, not open action items.
        in_closed_section = False
        for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            # Update section tracking on every H2 boundary.
            if h2_re.match(line):
                in_closed_section = bool(CLOSED_SECTION_HEADINGS_RE.match(line))
                continue
            if in_closed_section:
                continue
            if row_re.search(line) is None:
                continue
            # Skip rows that carry an explicit closed-date stamp or "closed: "
            # marker — these are closed items that haven't yet moved to a
            # dedicated section (BACKLOG/OPEN inline-close pattern).
            if CLOSED_ROW_RE.search(line):
                continue
            blocked, reason = _blocked_reason(line)
            severity = 66 if not blocked else 38
            evidence = " ".join(line.strip().split())
            findings.append(
                Finding(
                    id=_finding_id("state_row", rel, line_no, evidence),
                    kind="state_row",
                    severity=severity,
                    area="state",
                    path=rel,
                    line=line_no,
                    title="tracked local state row",
                    evidence=evidence[:240],
                    action="Keep this row synced with PR/commit history; promote unblocked rows into code work.",
                    blocked=blocked,
                    blocked_reason=reason,
                )
            )
    return findings


def scan_smoke_models(repo_root: Path) -> list[Finding]:
    registry = repo_root / "model" / "tiny" / "registry.json"
    if not registry.is_file():
        return []
    try:
        payload = json.loads(registry.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return []
    findings: list[Finding] = []
    for idx, model in enumerate(payload.get("models", []), start=1):
        if not isinstance(model, dict) or model.get("smoke") is not True:
            continue
        model_id = str(model.get("id", "unknown"))
        evidence = f"{model_id} is still marked smoke=true in model/tiny/registry.json"
        blocked = any(token in model_id for token in ("placeholder", "smoke"))
        findings.append(
            Finding(
                id=_finding_id("smoke_model", "model/tiny/registry.json", idx, evidence),
                kind="smoke_model",
                severity=42 if blocked else 70,
                area="models",
                path="model/tiny/registry.json",
                line=idx,
                title=f"smoke model registry row: {model_id}",
                evidence=evidence,
                action="Promote with real corpus evidence, or leave explicitly documented as smoke-only.",
                blocked=blocked,
                blocked_reason="smoke/placeholder fixture" if blocked else "",
            )
        )
    return findings


def scan_script_clusters(repo_root: Path) -> list[Finding]:
    scripts_dir = repo_root / "ai" / "scripts"
    if not scripts_dir.is_dir():
        return []
    groups: dict[str, list[Path]] = defaultdict(list)
    for path in sorted(scripts_dir.glob("*.py")):
        prefix = path.stem.split("_", 1)[0]
        if prefix in {"train", "export", "eval", "extract", "validate"}:
            groups[prefix].append(path)
    findings: list[Finding] = []
    for prefix, paths in sorted(groups.items()):
        if len(paths) < SCRIPT_CLUSTER_MIN_COUNT:
            continue
        rels = ", ".join(_relative(repo_root, path) for path in paths[:SCRIPT_CLUSTER_MIN_COUNT])
        evidence = f"{len(paths)} ai/scripts/{prefix}_*.py entry points; examples: {rels}"
        findings.append(
            Finding(
                id=_finding_id("script_cluster", f"ai/scripts/{prefix}_*.py", None, evidence),
                kind="script_cluster",
                severity=min(88, 48 + len(paths) * 2),
                area="tiny-ai",
                path="ai/scripts",
                line=None,
                title=f"{prefix}_*.py script family has {len(paths)} entry points",
                evidence=evidence,
                action=(
                    "Audit for shared manifest/config plumbing before adding another one-off "
                    f"{prefix} script."
                ),
                blocked=False,
                blocked_reason="",
            )
        )
    return findings


def dedupe_findings(findings: Iterable[Finding]) -> list[Finding]:
    by_id: dict[str, Finding] = {}
    for finding in findings:
        by_id.setdefault(finding.id, finding)
    return sorted(
        by_id.values(),
        key=lambda finding: (-finding.severity, finding.blocked, finding.area, finding.path),
    )


def summarize_findings(findings: Sequence[Finding]) -> dict[str, object]:
    by_kind = Counter(finding.kind for finding in findings)
    by_area = Counter(finding.area for finding in findings)
    blocked = sum(1 for finding in findings if finding.blocked)
    return {
        "total": len(findings),
        "actionable": len(findings) - blocked,
        "blocked": blocked,
        "by_kind": dict(sorted(by_kind.items())),
        "by_area": dict(sorted(by_area.items())),
    }


def run_audit(
    repo_root: Path,
    *,
    roots: Sequence[str] = DEFAULT_SCAN_ROOTS,
    state_files: Sequence[str] = DEFAULT_STATE_FILES,
    include_archives: bool = False,
    max_per_file: int = 5,
) -> AuditReport:
    marker_findings = scan_marker_findings(
        repo_root,
        roots,
        include_archives=include_archives,
        max_per_file=max_per_file,
    )
    state_findings = scan_state_files(repo_root, state_files)
    smoke_findings = scan_smoke_models(repo_root)
    clusters = scan_script_clusters(repo_root)
    findings = dedupe_findings([*marker_findings, *state_findings, *smoke_findings])
    return AuditReport(repo_root=str(repo_root), findings=findings, clusters=clusters)


def _table(headers: Sequence[str], rows: Sequence[Sequence[str]]) -> str:
    if not rows:
        return "_None._"
    out = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    out.extend("| " + " | ".join(cell.replace("|", "\\|") for cell in row) + " |" for row in rows)
    return "\n".join(out)


def render_markdown(report: AuditReport, *, max_findings: int = 30) -> str:
    all_findings = report.findings + report.clusters
    summary = summarize_findings(all_findings)
    actionable = [finding for finding in all_findings if not finding.blocked]
    blocked = [finding for finding in all_findings if finding.blocked]
    top = sorted(actionable, key=lambda finding: -finding.severity)[:max_findings]
    blocked_top = sorted(blocked, key=lambda finding: -finding.severity)[
        : max(5, max_findings // 3)
    ]

    lines = [
        "# Project modernization audit",
        "",
        (
            "Read-only scan of repo code, human-facing docs, model registry metadata, "
            "and local state files."
        ),
        "",
        "## Summary",
        "",
        f"- Total findings: **{summary['total']}**",
        f"- Actionable now: **{summary['actionable']}**",
        f"- Blocked/deferred: **{summary['blocked']}**",
        "",
        "### Findings by area",
        "",
        _table(
            ["Area", "Count"],
            [[area, str(count)] for area, count in summary["by_area"].items()],
        ),
        "",
        "## Top Actionable Findings",
        "",
        _table(
            ["Score", "Area", "Kind", "Location", "Evidence", "Action"],
            [
                [
                    str(finding.severity),
                    finding.area,
                    finding.kind,
                    f"`{finding.path}`" + (f":{finding.line}" if finding.line else ""),
                    finding.evidence,
                    finding.action,
                ]
                for finding in top
            ],
        ),
        "",
        "## Modernization Clusters",
        "",
        _table(
            ["Score", "Area", "Cluster", "Evidence", "Action"],
            [
                [
                    str(finding.severity),
                    finding.area,
                    finding.title,
                    finding.evidence,
                    finding.action,
                ]
                for finding in sorted(report.clusters, key=lambda finding: -finding.severity)
            ],
        ),
        "",
        "## Blocked Or Deferred Findings",
        "",
        _table(
            ["Score", "Area", "Kind", "Location", "Reason", "Evidence"],
            [
                [
                    str(finding.severity),
                    finding.area,
                    finding.kind,
                    f"`{finding.path}`" + (f":{finding.line}" if finding.line else ""),
                    finding.blocked_reason,
                    finding.evidence,
                ]
                for finding in blocked_top
            ],
        ),
        "",
        "## Operating Notes",
        "",
        "- Treat this report as a queue-shaping tool, not a CI gate.",
        "- Review blocked rows before deleting or reprioritising them; the scanner is text-based.",
        "- Keep `.workingdir2/OPEN.md` and `.workingdir2/BACKLOG.md` as the editorial state of record.",
        "",
    ]
    return "\n".join(lines)


def _parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=REPO_ROOT,
        help=f"repository root (default: {REPO_ROOT})",
    )
    parser.add_argument(
        "--scan-root",
        action="append",
        default=None,
        help="root to scan; repeat to override defaults",
    )
    parser.add_argument(
        "--state-file",
        action="append",
        default=None,
        help="state file to scan for T-* rows; repeat to override defaults",
    )
    parser.add_argument("--out-json", type=Path, default=None, help="write JSON report")
    parser.add_argument("--out-md", type=Path, default=None, help="write Markdown report")
    parser.add_argument(
        "--max-findings",
        type=int,
        default=30,
        help="maximum actionable rows to render in Markdown",
    )
    parser.add_argument(
        "--max-per-file",
        type=int,
        default=5,
        help="maximum marker findings per scanned file",
    )
    parser.add_argument(
        "--include-archives",
        action="store_true",
        help="include paths containing an archive/ component",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(argv)
    repo_root = args.repo_root.resolve()
    roots = tuple(args.scan_root) if args.scan_root else DEFAULT_SCAN_ROOTS
    state_files = tuple(args.state_file) if args.state_file else DEFAULT_STATE_FILES
    report = run_audit(
        repo_root,
        roots=roots,
        state_files=state_files,
        include_archives=args.include_archives,
        max_per_file=args.max_per_file,
    )

    if args.out_json is not None:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(json.dumps(report.to_json(), indent=2) + "\n", encoding="utf-8")

    rendered = render_markdown(report, max_findings=args.max_findings)
    if args.out_md is not None:
        args.out_md.parent.mkdir(parents=True, exist_ok=True)
        args.out_md.write_text(rendered, encoding="utf-8")
    elif args.out_json is None:
        print(rendered, end="")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
