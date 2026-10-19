# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for check_netflix_unported.py (ADR-0339, ADR-3354)."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
from pathlib import Path

import pytest

SCRIPT_PATH = Path(__file__).resolve().parents[1] / "check_netflix_unported.py"
SPEC = importlib.util.spec_from_file_location("check_netflix_unported", SCRIPT_PATH)
if SPEC is None or SPEC.loader is None:
    raise ImportError(f"Cannot load module from {SCRIPT_PATH}")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
main = MODULE.main

EXIT_OK = 0
EXIT_CLEAN = 1
EXIT_ERROR = 2
EXPECTED_JSON_ITEMS = 2


def clean_git_environment() -> dict[str, str]:
    """Return the caller's environment without keys that start with "GIT_"."""
    environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    return environment


SAVED_GIT_ENVIRONMENT: dict[str, str] = {}


def setup_module() -> None:
    """Keep a hook's GIT_* away from the script's own git calls."""
    for key in [key for key in os.environ if key.startswith("GIT_")]:
        SAVED_GIT_ENVIRONMENT[key] = os.environ.pop(key)


def teardown_module() -> None:
    """Give the caller's GIT_* back."""
    os.environ.update(SAVED_GIT_ENVIRONMENT)
    SAVED_GIT_ENVIRONMENT.clear()


def git_cmd(repo: Path, *args: str) -> str:
    """Run git in *repo* with fixed user identity."""
    cmd = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t", *args]
    proc = subprocess.run(  # noqa: S603
        cmd, capture_output=True, text=True, check=True, env=clean_git_environment()
    )
    return proc.stdout.strip()


def init_repo(tmp_path: Path) -> tuple[Path, str]:
    """Initialize a git repository with a recorded parity pin in tmp_path."""
    git_cmd(tmp_path, "init", "-b", "master")
    git_cmd(tmp_path, "commit", "--allow-empty", "-m", "upstream pin commit")
    pin_sha = git_cmd(tmp_path, "rev-parse", "HEAD")

    bugs_doc = tmp_path / "docs" / "development" / "known-upstream-bugs.md"
    bugs_doc.parent.mkdir(parents=True, exist_ok=True)
    bugs_doc.write_text(
        f"# Known upstream bugs\n\n"
        f"## Upstream head the fork is at parity with: `{pin_sha}` (2026-10-02)\n",
        encoding="utf-8",
    )
    git_cmd(tmp_path, "add", "docs/development/known-upstream-bugs.md")
    git_cmd(tmp_path, "commit", "-m", "record parity pin")

    return tmp_path, pin_sha


def test_positive_unrecorded_commit_listed_and_pr_stripped(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """Positive test: one unrecorded commit after pin -> exit 0, (#12) stripped."""
    repo, _ = init_repo(tmp_path)
    git_cmd(repo, "commit", "--allow-empty", "-m", "libvmaf: add feature (#12)")
    head_sha = git_cmd(repo, "rev-parse", "HEAD")

    ledger = repo / "ledger.tsv"
    ledger.write_text("# header\n", encoding="utf-8")

    code = main(["--repo", str(repo), "--upstream-ref", "HEAD", "--ledger", str(ledger)])
    out, err = capsys.readouterr()
    assert code == EXIT_OK
    assert err == ""
    assert f"{head_sha} libvmaf: add feature" in out
    assert "(#12)" not in out

    # Test --json flag
    code_json = main(
        ["--repo", str(repo), "--upstream-ref", "HEAD", "--ledger", str(ledger), "--json"]
    )
    out_json, _ = capsys.readouterr()
    assert code_json == EXIT_OK
    data = json.loads(out_json)
    assert len(data) == EXPECTED_JSON_ITEMS  # "record parity pin" and "libvmaf: add feature"
    assert data[-1]["sha"] == head_sha
    assert data[-1]["subject"] == "libvmaf: add feature"


def test_negative_all_recorded_exits_one(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """Negative test: all commits after pin recorded in ledger -> exit 1."""
    repo, _ = init_repo(tmp_path)
    # The only commit after pin so far is "record parity pin"
    pin_doc_commit = git_cmd(repo, "rev-parse", "HEAD")

    ledger = repo / "ledger.tsv"
    ledger.write_text(f"{pin_doc_commit}\tported\t#100\n", encoding="utf-8")

    code = main(["--repo", str(repo), "--upstream-ref", "HEAD", "--ledger", str(ledger)])
    out, err = capsys.readouterr()
    assert code == EXIT_CLEAN
    assert out == ""
    assert err == ""

    # Test --json flag on empty unported
    code_json = main(
        ["--repo", str(repo), "--upstream-ref", "HEAD", "--ledger", str(ledger), "--json"]
    )
    out_json, _ = capsys.readouterr()
    assert code_json == EXIT_CLEAN
    assert out_json.strip() == "[]"


def test_boundary_merge_commit_and_pin_not_listed(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """Boundary test: pin itself not listed, merge commits excluded."""
    repo, pin_sha = init_repo(tmp_path)
    pin_doc_commit = git_cmd(repo, "rev-parse", "HEAD")

    # Create a feature branch and merge with --no-ff
    git_cmd(repo, "checkout", "-b", "feature")
    git_cmd(repo, "commit", "--allow-empty", "-m", "feature commit")
    feature_sha = git_cmd(repo, "rev-parse", "HEAD")
    git_cmd(repo, "checkout", "master")
    git_cmd(repo, "merge", "--no-ff", "feature", "-m", "merge branch feature")
    merge_sha = git_cmd(repo, "rev-parse", "HEAD")

    ledger = repo / "ledger.tsv"
    # Record pin_doc_commit and feature_sha
    ledger.write_text(
        f"{pin_doc_commit}\tported\t#101\n{feature_sha}\tported\t#102\n",
        encoding="utf-8",
    )

    code = main(["--repo", str(repo), "--upstream-ref", "HEAD", "--ledger", str(ledger)])
    out, _ = capsys.readouterr()
    assert code == EXIT_CLEAN
    assert pin_sha not in out
    assert merge_sha not in out


LEDGER_REFUSALS: list[tuple[str, str]] = [
    (
        "1111111111111111111111111111111111111111\tported\t#1\n"
        "1111111111111111111111111111111111111111\tported\t#2\n",
        "duplicate commit sha",
    ),
    (
        "123badsha\tported\t#1\n",
        "invalid commit sha",
    ),
    (
        "1111111111111111111111111111111111111111\tinvalid-state\t#1\n",
        "unknown state",
    ),
    (
        "1111111111111111111111111111111111111111\tported\t   \n",
        "empty reference",
    ),
    (
        "1111111111111111111111111111111111111111\tported\n",
        "expected 3 tab-separated columns",
    ),
]


def test_ledger_refusals(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    """Ledger refusals: duplicate, bad sha, unknown state, empty ref -> exit 2."""
    for index, (ledger_content, expected_error) in enumerate(LEDGER_REFUSALS):
        case_dir = tmp_path / f"case{index}"
        case_dir.mkdir()
        repo, _ = init_repo(case_dir)
        ledger = repo / "ledger.tsv"
        ledger.write_text(ledger_content, encoding="utf-8")

        code = main(["--repo", str(repo), "--upstream-ref", "HEAD", "--ledger", str(ledger)])
        _, err = capsys.readouterr()
        assert code == EXIT_ERROR, expected_error
        assert expected_error in err


def test_planted_sha_not_in_range_accepted_and_unrecorded_reported(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """PLANTED case: ledger row with sha outside range accepted, unrecorded reported."""
    repo, _ = init_repo(tmp_path)
    git_cmd(repo, "commit", "--allow-empty", "-m", "real unrecorded commit")
    real_sha = git_cmd(repo, "rev-parse", "HEAD")

    # Planted SHA: arbitrary valid 40-hex lowercase SHA not in git repo
    planted_sha = "a" * 40
    ledger = repo / "ledger.tsv"
    ledger.write_text(f"{planted_sha}\tported\t#9999\n", encoding="utf-8")

    code = main(["--repo", str(repo), "--upstream-ref", "HEAD", "--ledger", str(ledger)])
    out, err = capsys.readouterr()
    assert code == EXIT_OK
    assert err == ""
    assert real_sha in out


def test_infra_errors_exit_two(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    """Infrastructure errors (missing ref, bad pin, missing file) exit with 2."""
    repo, _ = init_repo(tmp_path)
    ledger = repo / "ledger.tsv"
    ledger.write_text("# header\n", encoding="utf-8")

    # Missing upstream ref
    code = main(["--repo", str(repo), "--upstream-ref", "nonexistent-ref", "--ledger", str(ledger)])
    _, err = capsys.readouterr()
    assert code == EXIT_ERROR
    assert "not found" in err

    # Missing ledger
    code = main(
        ["--repo", str(repo), "--upstream-ref", "HEAD", "--ledger", str(repo / "missing.tsv")]
    )
    _, err = capsys.readouterr()
    assert code == EXIT_ERROR
    assert "ledger file not found" in err

    # Missing known-upstream-bugs.md
    (repo / "docs" / "development" / "known-upstream-bugs.md").unlink()
    code = main(["--repo", str(repo), "--upstream-ref", "HEAD", "--ledger", str(ledger)])
    _, err = capsys.readouterr()
    assert code == EXIT_ERROR
    assert "record file not found" in err
