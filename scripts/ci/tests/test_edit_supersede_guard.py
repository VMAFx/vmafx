#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise the edit supersede guard and its required-job wiring."""

import json
import re
import secrets
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Callable

import pytest
import yaml  # type: ignore[import-untyped]  # no PyYAML stubs in the hook environment

from scripts.ci import ci_tier
from scripts.ci import edit_supersede_guard as guard
from scripts.ci.test_sycl_tidy_workflow_contract import extract_job_block

ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = ROOT / ".github/workflows/rule-enforcement.yml"
ACTION = ROOT / ".github/actions/edit-supersede-guard/action.yml"
USE = "./.github/actions/edit-supersede-guard"
STAMP = "2026-11-17T06:05:00Z"
PR = {"title": "title", "body": "body", "base": {"ref": "master"}}


def findings(workflow_text: str, action_text: str) -> list[str]:
    """Check marked jobs and the composite action using existing YAML and marker conventions."""
    workflow = yaml.safe_load(workflow_text)
    errors = []
    for job_id, job in workflow["jobs"].items():
        block = extract_job_block(workflow_text, job_id)
        if not re.search(r"^\s*#\s*required-aggregator\s*$", block, re.MULTILINE):
            continue
        steps = job.get("steps", [])
        checkout = next(
            (
                i
                for i, step in enumerate(steps)
                if str(step.get("uses", "")).startswith("actions/checkout@")
            ),
            -1,
        )
        if checkout < 0 or checkout + 1 >= len(steps) or steps[checkout + 1].get("uses") != USE:
            errors.append(f"{job_id}: guard must follow checkout")
    action = yaml.safe_load(action_text)
    steps = action["runs"]["steps"]
    if not any(
        "github.event.action == 'edited'" in str(step.get("if", ""))
        and step.get("run") == "python3 scripts/ci/edit_supersede_guard.py"
        for step in steps
    ):
        errors.append("action: missing edited guard or script")
    return errors


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
    ("change", "expected"),
    [
        ({}, []),
        ({"title": "new"}, ["title"]),
        ({"body": "new"}, ["body"]),
        ({"base": {"ref": "next"}}, ["base.ref"]),
    ],
)
def test_decide_fields(change: dict[str, object], expected: list[str]) -> None:
    assert guard.decide(PR, {**PR, **change}) == expected


def test_body_none_and_empty() -> None:
    assert guard.decide({**PR, "body": None}, {**PR, "body": ""}) == []
    assert guard.decide({**PR, "body": None}, {**PR, "body": "x"}) == ["body"]


def test_settle_bounds() -> None:
    stamp = datetime.fromisoformat(STAMP.replace("Z", "+00:00"))
    assert guard.settle_wait_s(STAMP, stamp + timedelta(seconds=2)) == guard.SETTLE_S - 2
    assert guard.settle_wait_s(STAMP, stamp + timedelta(seconds=60)) == 0
    assert guard.settle_wait_s(STAMP, stamp - timedelta(seconds=120)) == guard.MAX_WAIT_S


def event_env(tmp_path: Path, *, action: str = "edited", stamp: str = STAMP) -> dict[str, str]:
    event = tmp_path / "event.json"
    event.write_text(
        json.dumps({"action": action, "number": 7, "pull_request": {**PR, "updated_at": stamp}})
    )
    return {
        "GITHUB_EVENT_PATH": str(event),
        "GITHUB_REPOSITORY": "org/repo",
        "GH_TOKEN": secrets.token_urlsafe(8),
    }


def call(env: dict[str, str], fetch: Callable[..., object]) -> int:
    return guard.run(env, fetch, lambda _: None, lambda: datetime(2026, 11, 17, 6, 5, tzinfo=UTC))


def test_bad_stamp_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    assert call(event_env(tmp_path, stamp="bad"), lambda *args: PR) == 1
    assert "::error::" in capsys.readouterr().out


def test_non_edited_skips_fetch(tmp_path: Path) -> None:
    def unexpected(*args: object) -> object:
        raise AssertionError("fetch called")

    assert call(event_env(tmp_path, action="opened"), unexpected) == 0


def test_fetch_error_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    def fail(*args: object) -> object:
        raise ci_tier.TierError("failure")

    assert call(event_env(tmp_path), fail) == 1
    assert "::error::" in capsys.readouterr().out


def test_superseded_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    assert call(event_env(tmp_path), lambda *args: {**PR, "body": "new"}) == 1
    assert (
        f"::error::superseded by a later edit: body changed since the event at {STAMP}; the Rules run of the later edit judges the current text"
        in capsys.readouterr().out
    )


def test_equal_passes(tmp_path: Path) -> None:
    assert call(event_env(tmp_path), lambda *args: PR) == 0


def test_workflow_contract() -> None:
    assert findings(WORKFLOW.read_text(), ACTION.read_text()) == []


def test_contract_missing_guard() -> None:
    workflow = "jobs:\n  example:\n    # required-aggregator\n    steps:\n      - uses: actions/checkout@sha\n"
    assert "example" in findings(workflow, ACTION.read_text())[0]


def test_contract_guard_before_checkout() -> None:
    workflow = f"jobs:\n  example:\n    # required-aggregator\n    steps:\n      - uses: {USE}\n      - uses: actions/checkout@sha\n"
    assert "example" in findings(workflow, ACTION.read_text())[0]


def test_contract_action_missing_if() -> None:
    action = ACTION.read_text().replace(
        "      if: github.event_name == 'pull_request' && github.event.action == 'edited'\n", ""
    )
    assert "action" in findings(WORKFLOW.read_text(), action)[0]


@pytest.mark.parametrize("missing", ["number", "stamp", "token", "repository"])  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
def test_missing_event_data_fails_closed(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], missing: str
) -> None:
    env = event_env(tmp_path)
    event = Path(env["GITHUB_EVENT_PATH"])
    payload = json.loads(event.read_text())
    if missing == "number":
        del payload["number"]
    elif missing == "stamp":
        del payload["pull_request"]["updated_at"]
    elif missing == "token":
        del env["GH_TOKEN"]
    else:
        del env["GITHUB_REPOSITORY"]
    event.write_text(json.dumps(payload))
    assert call(env, lambda *args: pytest.fail("unexpected fetch")) == 1
    assert "::error::" in capsys.readouterr().out


def test_invalid_payload_fails_closed(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    env = event_env(tmp_path)
    Path(env["GITHUB_EVENT_PATH"]).write_text("{")
    assert call(env, lambda *args: pytest.fail("unexpected fetch")) == 1
    assert "::error::" in capsys.readouterr().out
