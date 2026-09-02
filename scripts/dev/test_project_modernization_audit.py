# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for the project-modernization audit helper."""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

# pylint: disable=wrong-import-position
import project_modernization_audit as audit


def _write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def test_marker_scan_ranks_unblocked_stubs(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "tools" / "vmaf-tune" / "src" / "vmaftune" / "thing.py",
        "# TODO: replace scaffold path\nraise NotImplementedError('stub')\n",
    )

    findings = audit.scan_marker_findings(tmp_path, [str(source.relative_to(tmp_path))])

    assert findings
    assert findings[0].path == str(source.relative_to(tmp_path))
    assert any(finding.kind == "not_implemented" for finding in findings)
    assert all(not finding.blocked for finding in findings)


def test_marker_scan_ignores_historical_notimplemented_prose(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "ai" / "scripts" / "qat_train.py",
        '"""Replaces the NotImplementedError scaffold from the old PR."""\n',
    )

    findings = audit.scan_marker_findings(tmp_path, [str(source.relative_to(tmp_path))])

    assert findings == []


def test_marker_scan_ignores_exception_handlers_and_exception_types(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "tools" / "vmaf-tune" / "src" / "vmaftune" / "cli.py",
        "class TwoPassUnsupportedError(NotImplementedError):\n    pass\ntry:\n    run_plan()\nexcept NotImplementedError as exc:\n    raise RuntimeError(str(exc)) from exc\n",
    )

    findings = audit.scan_marker_findings(tmp_path, [str(source.relative_to(tmp_path))])

    assert findings == []


def test_marker_scan_keeps_live_notimplemented_raise(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "tools" / "vmaf-tune" / "src" / "vmaftune" / "gap.py",
        "def run():\n    raise NotImplementedError('real gap')\n",
    )

    findings = audit.scan_marker_findings(tmp_path, [str(source.relative_to(tmp_path))])

    assert len(findings) == 1
    assert findings[0].kind == "not_implemented"
    not_implemented_severity = next(
        severity for kind, _pattern, severity in audit.MARKERS if kind == "not_implemented"
    )
    assert findings[0].severity == not_implemented_severity


def test_marker_scan_ignores_notimplemented_in_markdown_docs(tmp_path: Path) -> None:
    doc = _write(
        tmp_path / "docs" / "development" / "audit.md",
        "A live `raise NotImplementedError(...)` is still a Python-source finding.\n",
    )

    findings = audit.scan_marker_findings(tmp_path, [str(doc.relative_to(tmp_path))])

    assert findings == []


def test_marker_scan_ignores_documented_enosys_contracts(tmp_path: Path) -> None:
    source = _write(
        tmp_path / ".github" / "workflows" / "build.yml",
        "# Stub-only build: every assertion pins the -ENOSYS contract.\n",
    )
    doc = _write(
        tmp_path / "docs" / "api" / "dnn.md",
        "`-ENOSYS` means this build was compiled without DNN support.\n",
    )

    findings = audit.scan_marker_findings(
        tmp_path,
        [str(source.relative_to(tmp_path)), str(doc.relative_to(tmp_path))],
    )

    assert findings == []


def test_marker_scan_keeps_live_enosys_without_contract_context(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "libvmaf" / "src" / "feature" / "missing.c",
        "int run(void)\n{\n    return -ENOSYS;\n}\n",
    )

    findings = audit.scan_marker_findings(tmp_path, [str(source.relative_to(tmp_path))])

    assert len(findings) == 1
    assert findings[0].kind == "enosys"


def test_marker_scan_ignores_optional_backend_enosys_contracts(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "libvmaf" / "src" / "feature" / "hip" / "integer_motion_hip.c",
        "/* Without HAVE_HIPCC, every lifecycle helper returns -ENOSYS so the\n * feature engine falls through to the CPU implementation. */\nint run(void)\n{\n    return -ENOSYS;\n}\n",
    )

    findings = audit.scan_marker_findings(tmp_path, [str(source.relative_to(tmp_path))])

    assert findings == []


def test_marker_scan_ignores_optional_runtime_error_translation(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "libvmaf" / "src" / "feature" / "hip" / "float_psnr_hip.c",
        "static int hip_err(hipError_t rc)\n{\n    switch (rc) {\n    case hipErrorNotSupported:\n        return -ENOSYS;\n    default:\n        return -EIO;\n    }\n}\n",
    )

    findings = audit.scan_marker_findings(tmp_path, [str(source.relative_to(tmp_path))])

    assert findings == []


def test_marker_scan_ignores_hip_scaffold_posture_else_branch(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "libvmaf" / "src" / "feature" / "hip" / "integer_psnr_hip.c",
        '#else\n    /* Scaffold posture: returns -ENOSYS ("runtime not ready"). */\n    return -ENOSYS;\n#endif /* HAVE_HIPCC */\n',
    )

    findings = audit.scan_marker_findings(tmp_path, [str(source.relative_to(tmp_path))])

    assert findings == []


def test_marker_scan_ignores_optional_loader_enosys(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "libvmaf" / "src" / "vulkan" / "common.c",
        "static int load_volk_once(void)\n{\n    VkResult vkr = volkInitialize();\n    if (vkr != VK_SUCCESS)\n        return -ENOSYS;\n    return 0;\n}\n",
    )

    findings = audit.scan_marker_findings(tmp_path, [str(source.relative_to(tmp_path))])

    assert findings == []


def test_marker_scan_ignores_test_double_stub_prose(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "tools" / "vmaf-tune" / "src" / "vmaftune" / "encode.py",
        '"""The runner argument lets unit tests substitute a stub subprocess."""\n',
    )

    findings = audit.scan_marker_findings(tmp_path, [str(source.relative_to(tmp_path))])

    assert findings == []


def test_marker_scan_ignores_adr_allocator_stub_files(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "scripts" / "adr" / "README.md",
        "This removes both the `.md.stub` file and the reservation stub file.\n",
    )

    findings = audit.scan_marker_findings(tmp_path, [str(source.relative_to(tmp_path))])

    assert findings == []


def test_marker_scan_skips_hyphenated_test_helpers(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "scripts" / "adr" / "test-next-free.sh",
        "# Clean up any stubs we create, even on early exit.\n",
    )

    findings = audit.scan_marker_findings(tmp_path, ["scripts"])

    assert source.is_file()
    assert findings == []


def test_marker_scan_ignores_non_implementation_stub_terms(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "libvmaf" / "src" / "dnn" / "dnn_api.c",
        "/* Stub signature must match the real-ORT path declared in the header. */\n",
    )
    package = _write(
        tmp_path / "ai" / "pyproject.toml",
        '"pandas-stubs>=3.0.0.260204",\n',
    )

    findings = audit.scan_marker_findings(
        tmp_path,
        [str(source.relative_to(tmp_path)), str(package.relative_to(tmp_path))],
    )

    assert findings == []


def test_blocked_state_row_is_classified(tmp_path: Path) -> None:
    state = _write(
        tmp_path / ".workingdir2" / "OPEN.md",
        "- **T-HDR-VMAF-MODEL** — blocked on upstream Netflix releases.\n",
    )

    findings = audit.scan_state_files(tmp_path, [str(state.relative_to(tmp_path))])

    assert len(findings) == 1
    assert findings[0].blocked
    assert findings[0].blocked_reason.lower() == "blocked"


# ---------------------------------------------------------------------------
# False-positive category 1: developer-tools placeholder noise
# The calibration scripts use "placeholder" as a domain STATUS value.
# ---------------------------------------------------------------------------


def test_calibration_scripts_placeholder_is_suppressed(tmp_path: Path) -> None:
    """placeholder in calibration files should not fire as a gap marker."""
    # Mirrors the content pattern in scripts/ci/gpu_ulp_calibration.yaml.
    yaml_content = (
        "# A placeholder entry uses the gate defaults until calibrated.\n"
        "- gpu_id_pattern: RTX-4090\n"
        "  status: placeholder\n"
        "  tolerance: 1e-5\n"
    )
    cal_py_content = (
        '    status: str  # "calibrated" | "placeholder"\n'
        '    label_kind = "calibrated" if entry.status == "calibrated" else "placeholder"\n'
        '    return feature_default, f"placeholder-default:{entry.gpu_id_pattern}"\n'
    )
    _write(tmp_path / "scripts" / "ci" / "gpu_ulp_calibration.yaml", yaml_content)
    _write(tmp_path / "scripts" / "ci" / "cross_backend_calibration.py", cal_py_content)
    _write(tmp_path / "scripts" / "ci" / "cross_backend_parity_gate.py", cal_py_content)

    findings = audit.scan_marker_findings(
        tmp_path,
        ["scripts/ci"],
        max_per_file=20,
    )
    placeholder_findings = [f for f in findings if f.kind == "placeholder"]

    assert placeholder_findings == [], (
        "placeholder marker should be suppressed in calibration files; "
        f"got {placeholder_findings}"
    )


def test_calibration_scripts_other_markers_still_fire(tmp_path: Path) -> None:
    """Other markers (e.g. TODO) should still fire in calibration files."""
    content = "# TODO: add more placeholder rows once data is collected\n"
    _write(tmp_path / "scripts" / "ci" / "cross_backend_calibration.py", content)

    findings = audit.scan_marker_findings(tmp_path, ["scripts/ci"])
    todo_findings = [f for f in findings if f.kind == "todo"]

    assert len(todo_findings) == 1, "TODO should still be reported in calibration files"


def test_non_calibration_script_placeholder_still_fires(tmp_path: Path) -> None:
    """placeholder in a non-calibration script path should still produce a finding."""
    _write(
        tmp_path / "scripts" / "dev" / "some_helper.py",
        "METRIC_TABLE = {'foo': 'placeholder'}  # TODO: real impl\n",
    )

    findings = audit.scan_marker_findings(tmp_path, ["scripts/dev"])
    placeholder_findings = [f for f in findings if f.kind == "placeholder"]

    assert (
        len(placeholder_findings) >= 1
    ), "placeholder in a non-calibration path should still be reported"


# ---------------------------------------------------------------------------
# False-positive category 2: state.md / BACKLOG / OPEN closed-row noise
# Rows under "Recently closed", "Resolved", or "Confirmed not-affected"
# headings and rows with "closed: " markers should be skipped.
# ---------------------------------------------------------------------------


def test_state_recently_closed_section_is_skipped(tmp_path: Path) -> None:
    """T-* rows under ## Recently closed should not produce findings."""
    content = (
        "## Open bugs\n"
        "\n"
        "| **T-OPEN-BUG-ACTIVE** | Some description. |\n"
        "\n"
        "## Recently closed\n"
        "\n"
        "| **T-OLD-BUG-FIXED** | Fixed by PR #42. | (2026-05-01) |\n"
        "| **T-ANOTHER-CLOSED** | Also resolved. | (2026-05-15) |\n"
        "\n"
        "## Deferred (waiting on external trigger)\n"
        "\n"
        "| **T-DEFERRED-ITEM** | Blocked on upstream. |\n"
    )
    state = _write(tmp_path / "docs" / "state.md", content)

    findings = audit.scan_state_files(tmp_path, [str(state.relative_to(tmp_path))])
    paths_and_lines = [(f.path, f.evidence) for f in findings]

    closed_findings = [
        e for _, e in paths_and_lines if "T-OLD-BUG-FIXED" in e or "T-ANOTHER-CLOSED" in e
    ]
    assert closed_findings == [], f"closed rows should be skipped; got: {closed_findings}"
    open_evidence = [e for _, e in paths_and_lines if "T-OPEN-BUG-ACTIVE" in e]
    assert len(open_evidence) == 1, "open bug should still be reported"


def test_state_confirmed_not_affected_section_is_skipped(tmp_path: Path) -> None:
    """T-* rows under ## Confirmed not-affected should not produce findings."""
    content = (
        "## Open bugs\n"
        "\n"
        "| **T-REAL-OPEN** | Real open work. |\n"
        "\n"
        "## Confirmed not-affected (or already-fixed upstream of the fork's master)\n"
        "\n"
        "| **T-CONFIRMED-NOAFFECT** | Upstream already fixed. |\n"
    )
    state = _write(tmp_path / "docs" / "state.md", content)

    findings = audit.scan_state_files(tmp_path, [str(state.relative_to(tmp_path))])
    evidence_list = [f.evidence for f in findings]

    assert not any(
        "T-CONFIRMED-NOAFFECT" in e for e in evidence_list
    ), "confirmed-not-affected rows should be suppressed"
    assert any("T-REAL-OPEN" in e for e in evidence_list), "real open rows should still be reported"


def test_state_closed_date_stamp_row_is_skipped(tmp_path: Path) -> None:
    """Rows with a (YYYY-MM-DD) date stamp at the end should be skipped."""
    content = (
        "## Open bugs\n"
        "\n"
        "| **T-STILL-OPEN** | Active issue. |\n"
        "| **T-INLINE-CLOSED** | Was fixed. | (2026-05-30) |\n"
    )
    state = _write(tmp_path / ".workingdir2" / "OPEN.md", content)

    findings = audit.scan_state_files(tmp_path, [str(state.relative_to(tmp_path))])
    evidence_list = [f.evidence for f in findings]

    assert not any(
        "T-INLINE-CLOSED" in e for e in evidence_list
    ), "date-stamped closed rows should be suppressed"
    assert any(
        "T-STILL-OPEN" in e for e in evidence_list
    ), "non-stamped open rows should still be reported"


def test_state_closed_marker_row_is_skipped(tmp_path: Path) -> None:
    """Rows with a literal ``closed: `` prefix should be skipped."""
    content = (
        "## Open bugs\n"
        "\n"
        "- **T-OPEN-ITEM** — active.\n"
        "- closed: **T-CLOSED-ITEM** — resolved in PR #99.\n"
    )
    state = _write(tmp_path / ".workingdir2" / "BACKLOG.md", content)

    findings = audit.scan_state_files(tmp_path, [str(state.relative_to(tmp_path))])
    evidence_list = [f.evidence for f in findings]

    assert not any("T-CLOSED-ITEM" in e for e in evidence_list)
    assert any("T-OPEN-ITEM" in e for e in evidence_list)


def test_false_positive_rate_reduction_combined(tmp_path: Path) -> None:
    """Verifies ~100 false-positive findings are suppressed vs the baseline.

    Synthetic state file with 70 closed rows + calibration file with 15
    placeholder occurrences.  Baseline (no exclusions) would produce >=85
    false-positive findings; with the two exclude rules the count should
    drop to zero for those items.
    """
    # Build a state file with 35 "Recently closed" rows and 35 rows in
    # "Confirmed not-affected".
    closed_rows = "\n".join(
        f"| **T-CLOSED-{i:03d}** | Some closed item {i}. | (2026-05-{(i % 28) + 1:02d}) |"
        for i in range(35)
    )
    confirmed_rows = "\n".join(
        f"| **T-NOAFFECT-{i:03d}** | Not affecting the fork. |" for i in range(35)
    )
    state_content = (
        "## Open bugs\n\n"
        "| **T-REAL-OPEN-001** | The one real open item. |\n\n"
        "## Recently closed\n\n"
        f"{closed_rows}\n\n"
        "## Confirmed not-affected (or already-fixed upstream)\n\n"
        f"{confirmed_rows}\n"
    )
    state = _write(tmp_path / "docs" / "state.md", state_content)

    # Build a calibration file with 15 placeholder occurrences.
    cal_lines = "\n".join(f"  status: placeholder  # entry {i}" for i in range(15))
    cal_content = f"# Calibration table\n{cal_lines}\n"
    _write(tmp_path / "scripts" / "ci" / "gpu_ulp_calibration.yaml", cal_content)

    state_findings = audit.scan_state_files(tmp_path, [str(state.relative_to(tmp_path))])
    marker_findings = audit.scan_marker_findings(tmp_path, ["scripts/ci"], max_per_file=30)

    # Only the one real open row should survive from the state scan.
    assert len(state_findings) == 1, (
        f"expected 1 open state finding, got {len(state_findings)}: "
        f"{[f.evidence[:60] for f in state_findings]}"
    )
    placeholder_findings = [f for f in marker_findings if f.kind == "placeholder"]
    assert (
        placeholder_findings == []
    ), f"expected 0 placeholder findings from calibration yaml, got {len(placeholder_findings)}"


def test_smoke_model_registry_rows_are_reported(tmp_path: Path) -> None:
    registry = {
        "models": [
            {"id": "smoke_v0", "smoke": True},
            {"id": "fr_regressor_v1", "smoke": False},
        ]
    }
    _write(tmp_path / "model" / "tiny" / "registry.json", json.dumps(registry))

    findings = audit.scan_smoke_models(tmp_path)

    assert len(findings) == 1
    assert findings[0].kind == "smoke_model"
    assert "smoke_v0" in findings[0].evidence


def test_script_clusters_detect_large_one_off_families(tmp_path: Path) -> None:
    for idx in range(6):
        _write(tmp_path / "ai" / "scripts" / f"train_model_{idx}.py", "pass\n")

    clusters = audit.scan_script_clusters(tmp_path)

    assert len(clusters) == 1
    assert clusters[0].kind == "script_cluster"
    assert "6 ai/scripts/train_*.py" in clusters[0].evidence


def test_markdown_and_json_outputs(tmp_path: Path) -> None:
    _write(tmp_path / "ai" / "scripts" / "extract_gap.py", "# FIXME: deferred stub\n")
    report = audit.run_audit(
        tmp_path,
        roots=("ai",),
        state_files=(),
        max_per_file=3,
    )
    rendered = audit.render_markdown(report, max_findings=5)
    payload = report.to_json()

    assert "Project modernization audit" in rendered
    assert "Top Actionable Findings" in rendered
    assert payload["summary"]["total"] >= 1
    assert payload["findings"][0]["path"] == "ai/scripts/extract_gap.py"


def test_main_writes_reports(tmp_path: Path) -> None:
    _write(tmp_path / "docs" / "usage" / "feature.md", "Known limitations: scaffold.\n")
    out_json = tmp_path / "out" / "audit.json"
    out_md = tmp_path / "out" / "audit.md"

    rc = audit.main(
        [
            "--repo-root",
            str(tmp_path),
            "--scan-root",
            "docs/usage",
            "--state-file",
            "missing.md",
            "--out-json",
            str(out_json),
            "--out-md",
            str(out_md),
        ]
    )

    assert rc == 0
    assert json.loads(out_json.read_text(encoding="utf-8"))["summary"]["total"] >= 1
    assert "Known limitations" in out_md.read_text(encoding="utf-8")


# ---------------------------------------------------------------------------
# dedupe_findings — duplicate-ID suppression and sort order
# ---------------------------------------------------------------------------


def test_dedupe_findings_removes_duplicates(tmp_path: Path) -> None:
    source = _write(
        tmp_path / "ai" / "scripts" / "train_main.py",
        "raise NotImplementedError('gap')\n",
    )
    findings = audit.scan_marker_findings(tmp_path, [str(source.relative_to(tmp_path))])
    # Repeat the list to simulate duplicates
    duped = findings + findings
    deduped = audit.dedupe_findings(duped)
    assert len(deduped) == len(findings), "dedupe_findings must collapse identical IDs"


def test_dedupe_findings_sorts_by_descending_severity(tmp_path: Path) -> None:
    low = _write(
        tmp_path / "docs" / "api" / "low.md",
        "Known limitations: scaffold.\n",
    )
    high = _write(
        tmp_path / "tools" / "vmaf-tune" / "src" / "vmaftune" / "high.py",
        "raise NotImplementedError('real gap')\n",
    )
    findings = audit.scan_marker_findings(
        tmp_path,
        [str(low.relative_to(tmp_path)), str(high.relative_to(tmp_path))],
    )
    deduped = audit.dedupe_findings(findings)
    severities = [f.severity for f in deduped]
    assert severities == sorted(severities, reverse=True)


# ---------------------------------------------------------------------------
# summarize_findings — counter aggregation
# ---------------------------------------------------------------------------


def test_summarize_findings_counts_total_and_blocked(tmp_path: Path) -> None:
    source_blocked = _write(
        tmp_path / ".workingdir2" / "OPEN.md",
        "- **T-HDR-VMAF** — blocked on upstream Netflix releases.\n",
    )
    source_open = _write(
        tmp_path / "ai" / "scripts" / "train_main.py",
        "raise NotImplementedError('real gap')\n",
    )
    findings = audit.scan_marker_findings(
        tmp_path, [str(source_open.relative_to(tmp_path))]
    ) + audit.scan_state_files(tmp_path, [str(source_blocked.relative_to(tmp_path))])

    summary = audit.summarize_findings(findings)
    assert summary["total"] == len(findings)
    assert summary["blocked"] >= 1
    assert summary["actionable"] == summary["total"] - summary["blocked"]
    assert isinstance(summary["by_kind"], dict)
    assert isinstance(summary["by_area"], dict)


def test_summarize_findings_empty_returns_zeros() -> None:
    summary = audit.summarize_findings([])
    assert summary["total"] == 0
    assert summary["blocked"] == 0
    assert summary["actionable"] == 0
    assert summary["by_kind"] == {}
    assert summary["by_area"] == {}


# ---------------------------------------------------------------------------
# _area_for — AREA_RULES prefix mapping
# ---------------------------------------------------------------------------


def test_area_for_vmaf_tune_path() -> None:
    assert audit._area_for("tools/vmaf-tune/src/foo.py") == "vmaf-tune"


def test_area_for_tiny_ai_path() -> None:
    assert audit._area_for("ai/scripts/train.py") == "tiny-ai"


def test_area_for_mcp_path() -> None:
    assert audit._area_for("mcp-server/vmaf-mcp/server.py") == "mcp"


def test_area_for_feature_extractor_path() -> None:
    assert audit._area_for("core/src/feature/vif.c") == "feature-extractors"


def test_area_for_ci_path() -> None:
    assert audit._area_for(".github/workflows/build.yml") == "ci"


def test_area_for_docs_path() -> None:
    assert audit._area_for("docs/usage/cli.md") == "docs"


def test_area_for_scripts_path() -> None:
    assert audit._area_for("scripts/dev/audit.py") == "developer-tools"


def test_area_for_unknown_path_returns_repo() -> None:
    assert audit._area_for("some/unknown/path/file.py") == "repo"


def test_area_for_exact_prefix_match() -> None:
    # "ai" must match "ai" itself (not just "ai/...")
    assert audit._area_for("ai") == "tiny-ai"


# ---------------------------------------------------------------------------
# render_markdown — clusters and blocked sections
# ---------------------------------------------------------------------------


def test_render_markdown_shows_modernization_clusters(tmp_path: Path) -> None:
    for idx in range(6):
        _write(tmp_path / "ai" / "scripts" / f"train_model_{idx}.py", "pass\n")
    report = audit.run_audit(tmp_path, roots=("ai",), state_files=())
    rendered = audit.render_markdown(report)
    assert "Modernization Clusters" in rendered


def test_render_markdown_shows_blocked_findings(tmp_path: Path) -> None:
    state = _write(
        tmp_path / ".workingdir2" / "OPEN.md",
        "- **T-HDR-VMAF** — blocked on upstream Netflix releases.\n",
    )
    report = audit.run_audit(tmp_path, roots=(), state_files=(str(state.relative_to(tmp_path)),))
    rendered = audit.render_markdown(report)
    assert "Blocked Or Deferred" in rendered


def test_render_markdown_findings_by_area_table(tmp_path: Path) -> None:
    _write(
        tmp_path / "ai" / "scripts" / "extract_gap.py",
        "raise NotImplementedError('gap')\n",
    )
    report = audit.run_audit(tmp_path, roots=("ai",), state_files=())
    rendered = audit.render_markdown(report)
    assert "Findings by area" in rendered


# ---------------------------------------------------------------------------
# _skip_path — include_archives branch
# ---------------------------------------------------------------------------


def test_skip_path_includes_archives_when_flag_set(tmp_path: Path) -> None:
    # The include_archives flag is exercised through the rglob branch of
    # _iter_files (i.e. when a directory root is passed, not a single file
    # path — the file-root branch yields directly without calling _skip_path).
    # Pass the top-level "src" directory so _iter_files uses rglob and
    # _skip_path is called for the archive/ subtree inside it.
    _write(
        tmp_path / "src" / "archive" / "train_enc.py",
        "raise NotImplementedError('missing encoder path')\n",
    )
    # Without include_archives: archive/ is in SKIP_PARTS → no findings.
    findings_excl = audit.scan_marker_findings(
        tmp_path,
        ["src"],
        include_archives=False,
    )
    assert findings_excl == [], "archive paths should be excluded by default"

    # With include_archives=True: archive/ is unblocked → finding fires.
    findings_incl = audit.scan_marker_findings(
        tmp_path,
        ["src"],
        include_archives=True,
    )
    assert len(findings_incl) >= 1, "archive paths should be included when flag is set"


# ---------------------------------------------------------------------------
# main() — stdout path (no --out-json / --out-md)
# ---------------------------------------------------------------------------


def test_main_prints_to_stdout_when_no_output_flags(tmp_path: Path, capsys) -> None:
    _write(tmp_path / "ai" / "scripts" / "train_main.py", "raise NotImplementedError('gap')\n")

    rc = audit.main(
        [
            "--repo-root",
            str(tmp_path),
            "--scan-root",
            "ai",
            "--state-file",
            "missing.md",
        ]
    )

    assert rc == 0
    captured = capsys.readouterr()
    assert "Project modernization audit" in captured.out
