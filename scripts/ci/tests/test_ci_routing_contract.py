#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Which jobs run for which event: the CI routing contract (ADR-2169).

Every case builds a synthetic GitHub event, works out with
``scripts/ci/ci_router.py`` which jobs of the real
``.github/workflows/*.yml`` run for it, and compares that with the tier
definition in ``.github/ci-tier.json``. The tier decision itself is the real
``scripts/ci/ci_tier.py``. The mutation cases at the end plant a defect in a
copy of the workflows and require the contract to catch it: a check that has
never been seen failing is not a check.

``CI_ROUTING_WORKFLOWS_DIR`` points the contract at another workflow
directory, for example a checkout of master from before ADR-2169, to show
which cases that tree fails.
"""

from __future__ import annotations

import datetime
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import ci_expressions
from scripts.ci import ci_router as wr
from scripts.ci.required_aggregator_harness import (
    AGGREGATOR_PATH,
    _embedded_script,
    _required_names,
)

ROOT = Path(__file__).resolve().parents[3]
CONFIG = ROOT / ".github" / "ci-tier.json"
WORKFLOWS = Path(os.environ.get("CI_ROUTING_WORKFLOWS_DIR", ROOT / ".github" / "workflows"))
REPOSITORY = "VMAFx/vmafx"
RELEASE_REF = "release-please--branches--master--components--vmafx"
# The self-hosted lanes report only when an operator provisioned a runner (ADR-1177,
# ADR-1319); the simulation reads their probe as an unprovisioned host.
HARDWARE_LANES = frozenset({"SYCL Parity (Arc A380)", "Coverage GPU"})
# A job of a push-triggered workflow whose own condition keeps it to pull requests.
PULL_REQUEST_ONLY_ON_PUSH = frozenset({"Dependency Review", "Scorecard PR Gate"})

OWN = wr.SyntheticEvent(
    "pull_request",
    "synchronize",
    ref="refs/pull/1/merge",
    head_ref="fix/example",
    head_repo=REPOSITORY,
)
FORK = wr.SyntheticEvent(
    "pull_request",
    "synchronize",
    ref="refs/pull/2/merge",
    head_ref="fix/example",
    head_repo="someone/vmafx",
)
RENOVATE = wr.SyntheticEvent(
    "pull_request",
    "synchronize",
    ref="refs/pull/3/merge",
    head_ref="renovate/typer-0.x",
    head_repo=REPOSITORY,
    author="renovate[bot]",
    author_type="Bot",
)
RELEASE = wr.SyntheticEvent(
    "pull_request",
    "synchronize",
    ref="refs/pull/4/merge",
    head_ref=RELEASE_REF,
    head_repo=REPOSITORY,
    author="github-actions[bot]",
    author_type="Bot",
)
HUMAN_ON_RELEASE_BRANCH = wr.SyntheticEvent(
    "pull_request",
    "synchronize",
    ref="refs/pull/5/merge",
    head_ref=RELEASE_REF,
    head_repo=REPOSITORY,
    author="someone",
)
MASTER_PUSH = wr.SyntheticEvent("push", ref="refs/heads/master")
FEATURE_PUSH = wr.SyntheticEvent("push", ref="refs/heads/fix/example")
TAG_PUSH = wr.SyntheticEvent("push", ref="refs/tags/v1.0.0")


def with_(event: wr.SyntheticEvent, **changes: object) -> wr.SyntheticEvent:
    values = {**event.__dict__, **changes}
    return wr.SyntheticEvent(**values)


class Contract:
    """The tier definition and the workflows it governs."""

    def __init__(self, directory: Path) -> None:
        self.directory = directory
        self.config = json.loads(CONFIG.read_text(encoding="utf-8"))
        self.workflows = wr.load_workflows(directory)
        script = _embedded_script(AGGREGATOR_PATH.read_text(encoding="utf-8"))
        self.required = _required_names(script)
        self.full_only = set(self.config["full_only"])
        self.always = set(self.config["always"])
        self.untiered = {(e["workflow"], e["job"]) for e in self.config["untiered_jobs"]}
        # ADR-2198: full-only contexts whose lane also runs on a light-tier pull request
        # when its own inputs changed (the planner's selectors decide, not the tier).
        self.lanes = {lane["context"]: lane for lane in self.config.get("own_input_lanes", [])}

    def run(self, event: wr.SyntheticEvent) -> tuple[list[str], list[wr.JobRun]]:
        return wr.simulate(self.workflows, event, CONFIG)

    def reporters(self, event: wr.SyntheticEvent) -> set[str]:
        """Required names reported by a job of a workflow the event triggers."""
        triggered, _ = self.run(event)
        reported = {name for file in triggered for name in _all_names(self.workflows[file])}
        return {name for name in self.required if name in reported}


def _all_names(workflow: dict[str, Any]) -> set[str]:
    names: set[str] = set()
    for job_id, job in workflow.get("jobs", {}).items():
        label = str(job.get("name", job_id))
        names.update(wr._expand(label, row) for row in wr._matrix_rows(job))
    return names


def expect_light_tier(contract: Contract, event: wr.SyntheticEvent) -> None:
    """A pull request from this repository runs the light tier and not the full-only contexts."""
    _, runs = contract.run(event)
    ran = wr.ran_names(runs)
    for name in sorted(contract.reporters(event)):
        owed = (
            name not in contract.full_only or name in contract.lanes
        ) and name not in HARDWARE_LANES
        if (name in ran) != owed:
            raise AssertionError(
                f"{event.head_ref}: {name!r} {'did not run' if owed else 'ran'} "
                f"on a light-tier pull request"
            )
    for name in sorted(contract.full_only - HARDWARE_LANES - set(contract.lanes)):
        if name in ran:
            raise AssertionError(
                f"{event.head_ref}: full-only {name!r} ran on a light-tier pull request"
            )
    if not any(name.startswith("CI tier (") for name in ran):
        raise AssertionError("the tier decision did not run")


def expect_own_input_lanes(contract: Contract, event: wr.SyntheticEvent) -> None:
    """A light-tier pull request runs each own-input lane's planner and its gate (ADR-2198).

    The simulation reads an unknown planner output as a planner selecting its work, so the
    legs run too; whether they run for a diff is the planner's and the selectors' contract
    (test_ci_impact.py, test_required_release_legs.py).
    """
    _, runs = contract.run(event)
    ran = wr.ran_names(runs)
    ran_job_ids = wr.ran_jobs(runs)
    if not contract.lanes:
        raise AssertionError("no own-input lane is declared")
    for name, lane in sorted(contract.lanes.items()):
        if name not in ran:
            raise AssertionError(f"{event.head_ref}: own-input lane {name!r} did not run")
        if (lane["workflow"], "impact") not in ran_job_ids:
            raise AssertionError(f"{event.head_ref}: the planner of {name!r} did not run")


def expect_own_input_lanes_idle(contract: Contract, event: wr.SyntheticEvent) -> None:
    """Without the light tier (a release pull request without the cut label) nothing of it runs."""
    _, runs = contract.run(event)
    ran = wr.ran_names(runs)
    for name in sorted(contract.lanes):
        if name in ran:
            raise AssertionError(f"release PR: own-input lane {name!r} ran without the cut label")


def expect_full_tier(contract: Contract, event: wr.SyntheticEvent) -> None:
    """The event runs every required context that has a reporter, hardware lanes aside."""
    _, runs = contract.run(event)
    ran = wr.ran_names(runs)
    for name in sorted(contract.reporters(event) - HARDWARE_LANES - PULL_REQUEST_ONLY_ON_PUSH):
        if name not in ran:
            raise AssertionError(
                f"{event.name} {event.head_ref or event.ref}: {name!r} did not run"
            )


def expect_release_light(contract: Contract, event: wr.SyntheticEvent) -> None:
    """The release pull request without the cut label runs the release contract only."""
    _, runs = contract.run(event)
    ran = wr.ran_names(runs)
    owed = contract.always
    for name in sorted(contract.reporters(event)):
        if (name in ran) != (name in owed):
            raise AssertionError(
                f"release PR: {name!r} {'did not run' if name in owed else 'ran'} without the cut label"
            )
    extra = wr.ran_jobs(runs) - contract.untiered - _tier_jobs(runs)
    if extra:
        raise AssertionError(f"release PR without the cut label ran {sorted(extra)}")


def _tier_jobs(runs: list[wr.JobRun]) -> set[tuple[str, str]]:
    return {(run.workflow, run.job) for run in runs if run.job == "tier" and run.ran}


def expect_nothing_but_exceptions(contract: Contract, event: wr.SyntheticEvent) -> None:
    """A draft pull request runs no job but the declared untiered ones."""
    _, runs = contract.run(event)
    extra = wr.ran_jobs(runs) - contract.untiered
    if extra:
        raise AssertionError(f"draft pull request ran {sorted(extra)}")


PRAETOR_MANAGED = (
    ("praetor-api.yml", "api-compatibility"),
    ("praetor-docs.yml", "documentation"),
    ("reuse.yml", "reuse"),
)
DRAFT_STOP = "github.event.pull_request.draft == true"
DRAFT_SKIP = "github.event.pull_request.draft != true"


def expect_draft_stop_before_any_work(contract: Contract, workflow: str, job: str) -> None:
    """A praetor-managed job does no work on a draft (praetor#815).

    The job starts, its first step fails closed, and every later step is skipped. The tier's
    job-level gate cannot be added to these byte-locked files, so the step shape is the proof.
    """
    steps = contract.workflows[workflow]["jobs"][job]["steps"]
    if steps[0].get("if") != DRAFT_STOP or "exit 1" not in steps[0].get("run", ""):
        raise AssertionError(f"{workflow}: the first step does not fail closed on a draft")
    unguarded = [s.get("name") for s in steps[1:] if s.get("if") != DRAFT_SKIP]
    if unguarded:
        raise AssertionError(f"{workflow}: steps {unguarded} run on a draft")


def expect_no_other_branch_runs(contract: Contract, event: wr.SyntheticEvent) -> None:
    """A push to a branch or a tag triggers nothing but the declared exceptions."""
    triggered, _ = contract.run(event)
    allowed = {e["workflow"] for e in contract.config["push_branch_exceptions"]}
    extra = set(triggered) - allowed
    if extra:
        raise AssertionError(f"{event.ref} triggers {sorted(extra)}")


def expect_no_jobs(contract: Contract, event: wr.SyntheticEvent) -> None:
    _, runs = contract.run(event)
    extra = wr.ran_jobs(runs)
    if extra:
        raise AssertionError(f"{event.label or event.action} event ran {sorted(extra)}")


def expect_escalation_only(contract: Contract, event: wr.SyntheticEvent) -> None:
    _, runs = contract.run(event)
    if wr.ran_jobs(runs) != {("pr-label-lanes.yml", "escalate")}:
        raise AssertionError(f"label {event.label!r} ran {sorted(wr.ran_jobs(runs))}")


# ADR-3279: the job that hosts the two MSVC GPU rows. One row is an own-input lane.
GPU_WORKFLOW = "libvmaf-build-matrix.yml"
GPU_JOB = "windows-gpu-build"
LEG_GATE = "steps.leg.outputs.run == 'true'"
SHELL_TIMEOUT_S = 30


def _gpu_job(contract: Contract) -> dict[str, Any]:
    job: dict[str, Any] = contract.workflows[GPU_WORKFLOW]["jobs"][GPU_JOB]
    return job


def expect_gpu_rows_tiered(contract: Contract) -> None:
    """Of the MSVC GPU rows only a declared own-input lane is a light leg (ADR-3279)."""
    rows = _gpu_job(contract)["strategy"]["matrix"]["include"]
    if not any(row["name"] in contract.lanes for row in rows):
        raise AssertionError(f"no row of {GPU_JOB!r} is an own-input lane")
    for row in rows:
        name = row["name"]
        if name not in contract.full_only:
            raise AssertionError(f"{name!r} is not a full-only context")
        expected = "light" if name in contract.lanes else "full"
        if row.get("tier") != expected:
            raise AssertionError(f"{name!r} carries tier {row.get('tier')!r}, not {expected!r}")


def expect_gpu_steps_gated(contract: Contract) -> None:
    """The first step decides for the leg; after the checkout no step runs without its answer."""
    steps = _gpu_job(contract)["steps"]
    if steps[0].get("id") != "leg" or "if" in steps[0]:
        raise AssertionError(f"the first step of {GPU_JOB!r} is not the unconditional leg decision")
    checkout = steps[1]
    if not str(checkout.get("uses", "")).startswith("actions/checkout@") or "if" in checkout:
        raise AssertionError(f"the second step of {GPU_JOB!r} is not the unconditional checkout")
    if checkout.get("with", {}).get("fetch-depth") != 1:
        raise AssertionError(f"the checkout of {GPU_JOB!r} is not shallow")
    for step in steps[2:]:
        if LEG_GATE not in str(step.get("if", "")):
            label = step.get("name") or step.get("uses")
            raise AssertionError(f"step {label!r} of {GPU_JOB!r} runs without the leg decision")


def gpu_leg_decision(contract: Contract, leg_tier: str, tier_full: str) -> str:
    """Run the decision step's own script and return what it writes for ``run``."""
    step = _gpu_job(contract)["steps"][0]
    bash = shutil.which("bash")
    if bash is None:
        raise unittest.SkipTest("no bash on this host; the hosted Windows leg runs the step")
    with tempfile.TemporaryDirectory(prefix="leg-decision-") as directory:
        output = Path(directory) / "github_output"
        output.touch()
        env = {"LEG_TIER": leg_tier, "TIER_FULL": tier_full, "GITHUB_OUTPUT": str(output)}
        subprocess.run(  # noqa: S603 -- resolved bash, the workflow's own step script
            [bash, "-c", str(step["run"])],
            env=env,
            check=True,
            capture_output=True,
            timeout=SHELL_TIMEOUT_S,
        )
        return output.read_text(encoding="utf-8").strip()


def expect_gpu_leg_decision(contract: Contract) -> None:
    """A light row runs on every tier that starts the job, a full row on the full tier only."""
    env = _gpu_job(contract)["steps"][0].get("env", {})
    wired = {"LEG_TIER": "${{ matrix.tier }}", "TIER_FULL": "${{ needs.tier.outputs.full }}"}
    if env != wired:
        raise AssertionError(f"the leg decision reads {env}, not {wired}")
    cases = (("light", "false", True), ("light", "true", True), ("full", "true", True))
    for leg_tier, tier_full, runs in (*cases, ("full", "false", False)):
        written = gpu_leg_decision(contract, leg_tier, tier_full)
        if written != f"run={'true' if runs else 'false'}":
            raise AssertionError(f"row tier {leg_tier}, full={tier_full}: wrote {written!r}")


def gpu_job_starts(
    contract: Contract, *, full: bool, planner: str, selected: bool, cancelled: bool = False
) -> bool:
    """Evaluate the job's own ``if:`` for a tier and a planner result (GitHub's needs context)."""
    planned = {"msvc-sycl": "true" if selected else "false"} if planner == "success" else {}
    tier = {"full": "true" if full else "false", "light": "true"}
    context = {
        "needs": {
            "tier": {"result": "success", "outputs": tier},
            "impact": {"result": planner, "outputs": planned},
        }
    }
    statuses = {
        "always": True,
        "cancelled": cancelled,
        "failure": planner == "failure",
        "success": planner == "success",
    }
    condition = str(_gpu_job(contract)["if"])
    return ci_expressions.truthy(ci_expressions.evaluate(condition, context, statuses))


def expect_gpu_job_condition(contract: Contract) -> None:
    """Full tier: always. Light tier: when the planner selects the leg, or failed (fail closed)."""
    job = _gpu_job(contract)
    if sorted(job.get("needs", [])) != ["impact", "tier"]:
        raise AssertionError(
            f"{GPU_JOB!r} needs {job.get('needs')!r}, not the tier and the planner"
        )
    cases = (
        ("full tier, leg selected", True, "success", True, False, True),
        ("full tier, leg not selected", True, "success", False, False, True),
        ("full tier, planner failed", True, "failure", False, False, True),
        ("light tier, leg selected", False, "success", True, False, True),
        ("light tier, leg not selected", False, "success", False, False, False),
        ("light tier, planner failed", False, "failure", False, False, True),
        ("light tier, planner skipped", False, "skipped", False, False, False),
        ("light tier, planner cancelled", False, "cancelled", False, False, False),
        ("run cancelled", True, "success", True, True, False),
    )
    for label, full, planner, selected, cancelled, starts in cases:
        got = gpu_job_starts(
            contract, full=full, planner=planner, selected=selected, cancelled=cancelled
        )
        if got != starts:
            raise AssertionError(f"{label}: the job {'started' if got else 'did not start'}")


class RoutingContract(unittest.TestCase):
    contract: Contract

    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = Contract(WORKFLOWS)

    def test_own_pull_request_runs_the_light_tier(self) -> None:
        expect_light_tier(self.contract, OWN)

    def test_own_input_lanes_run_on_a_light_tier_pull_request(self) -> None:
        expect_own_input_lanes(self.contract, OWN)
        expect_own_input_lanes(self.contract, RENOVATE)

    def test_own_input_lanes_do_not_run_on_the_release_pull_request(self) -> None:
        expect_own_input_lanes_idle(self.contract, RELEASE)

    def test_own_input_lanes_name_real_selectors_that_follow_their_own_paths(self) -> None:
        """The lane's work is selected by ci-impact.json selectors that never fire on a fallback."""
        selectors = json.loads((ROOT / ".github" / "ci-impact.json").read_text("utf-8"))[
            "selectors"
        ]
        for name, lane in self.contract.lanes.items():
            workflow = self.contract.workflows[lane["workflow"]]
            self.assertIn(name, _all_names(workflow))
            for selector in lane["selectors"]:
                with self.subTest(lane=name, selector=selector):
                    self.assertTrue(selectors[selector].get("own_paths_only"), selector)

    def test_ready_for_review_runs_the_same_tier(self) -> None:
        expect_light_tier(self.contract, with_(OWN, action="ready_for_review"))

    def test_renovate_pull_request_runs_the_light_tier(self) -> None:
        expect_light_tier(self.contract, RENOVATE)

    def test_fork_pull_request_runs_the_full_tier(self) -> None:
        expect_full_tier(self.contract, FORK)

    def test_full_label_runs_the_full_tier_on_an_own_pull_request(self) -> None:
        expect_full_tier(self.contract, with_(OWN, labels=("ci: full",)))

    def test_release_pull_request_without_the_cut_label_runs_the_contract_only(self) -> None:
        expect_release_light(self.contract, RELEASE)

    def test_release_pull_request_with_the_cut_label_runs_the_full_tier(self) -> None:
        expect_full_tier(self.contract, with_(RELEASE, labels=("autorelease: cut",)))

    def test_a_person_on_a_release_branch_name_runs_the_light_tier(self) -> None:
        """The head ref alone is not trusted (ADR-1151, ADR-1388)."""
        expect_light_tier(self.contract, HUMAN_ON_RELEASE_BRANCH)

    def test_cut_label_on_an_ordinary_pull_request_changes_nothing(self) -> None:
        expect_light_tier(self.contract, with_(OWN, labels=("autorelease: cut",)))

    def test_draft_pull_request_runs_no_job(self) -> None:
        expect_nothing_but_exceptions(self.contract, with_(OWN, draft=True))
        expect_nothing_but_exceptions(self.contract, with_(RENOVATE, draft=True))
        expect_nothing_but_exceptions(self.contract, with_(FORK, draft=True))
        expect_nothing_but_exceptions(self.contract, with_(RELEASE, draft=True))

    def test_praetor_managed_jobs_stop_on_a_draft_before_any_work(self) -> None:
        for workflow, job in PRAETOR_MANAGED:
            expect_draft_stop_before_any_work(self.contract, workflow, job)

    def test_master_push_runs_the_full_tier(self) -> None:
        expect_full_tier(self.contract, MASTER_PUSH)

    def test_push_to_another_branch_triggers_nothing(self) -> None:
        expect_no_other_branch_runs(self.contract, FEATURE_PUSH)

    def test_push_of_a_tag_triggers_nothing(self) -> None:
        expect_no_other_branch_runs(self.contract, TAG_PUSH)

    def test_unrelated_label_starts_no_job(self) -> None:
        expect_no_jobs(self.contract, with_(OWN, action="labeled", label="dependencies"))

    def test_escalation_labels_start_only_the_escalation(self) -> None:
        expect_escalation_only(self.contract, with_(OWN, action="labeled", label="ci: full"))
        release = with_(RELEASE, action="labeled", label="autorelease: cut")
        expect_escalation_only(self.contract, release)

    def test_escalation_label_on_a_fork_starts_nothing(self) -> None:
        expect_no_jobs(self.contract, with_(FORK, action="labeled", label="ci: full"))

    def test_every_pull_request_workflow_listens_for_ready_for_review(self) -> None:
        exempt = {"pr-type-label.yml"}
        for name, workflow in self.contract.workflows.items():
            filters = wr.triggers_of(workflow).get("pull_request")
            if filters is None or name in exempt:
                continue
            types = filters.get("types", wr.DEFAULT_TYPES["pull_request"])
            self.assertIn("ready_for_review", types, name)

    def test_tier_definition_names_only_required_contexts(self) -> None:
        for name in self.contract.full_only | self.contract.always:
            self.assertIn(name, self.contract.required)

    def test_release_contract_is_owed_by_every_tier(self) -> None:
        self.assertIn("Release Script Contract", self.contract.always)

    def test_matrix_legs_carry_the_tier_of_their_name(self) -> None:
        job = self.contract.workflows["libvmaf-build-matrix.yml"]["jobs"]["libvmaf-build"]
        for row in job["strategy"]["matrix"]["include"]:
            expected = "full" if row["name"] in self.contract.full_only else "light"
            self.assertEqual(row.get("tier"), expected, row["name"])

    def test_msvc_gpu_rows_carry_the_tier_of_their_lane(self) -> None:
        expect_gpu_rows_tiered(self.contract)

    def test_msvc_gpu_steps_wait_for_the_leg_decision(self) -> None:
        expect_gpu_steps_gated(self.contract)

    def test_msvc_gpu_leg_decision_follows_the_row_and_the_tier(self) -> None:
        expect_gpu_leg_decision(self.contract)

    def test_msvc_gpu_job_starts_on_selection_and_on_a_failed_planner(self) -> None:
        expect_gpu_job_condition(self.contract)

    def test_exceptions_are_unexpired(self) -> None:
        today = datetime.date.today()
        entries = (
            self.contract.config["untiered_jobs"] + self.contract.config["push_branch_exceptions"]
        )
        for entry in entries:
            self.assertTrue(entry["reason"].strip(), entry)
            self.assertGreaterEqual(datetime.date.fromisoformat(entry["expires"]), today, entry)

    def test_untiered_jobs_exist(self) -> None:
        for workflow, job in self.contract.untiered:
            self.assertIn(job, self.contract.workflows[workflow]["jobs"], (workflow, job))


class PlantedDefects(unittest.TestCase):
    """Each contract case must fail on a copy of the workflows with its defect planted."""

    def mutated(self, workflow: str, old: str, new: str) -> Contract:
        directory = Path(tempfile.mkdtemp(prefix="routing-mutation-"))
        self.addCleanup(shutil.rmtree, directory, True)
        for path in WORKFLOWS.glob("*.yml"):
            shutil.copy(path, directory / path.name)
        target = directory / workflow
        text = target.read_text(encoding="utf-8")
        self.assertIn(old, text, "the planted defect must apply")
        target.write_text(text.replace(old, new, 1), encoding="utf-8")
        return Contract(directory)

    def test_a_full_job_without_its_tier_gate_is_caught(self) -> None:
        contract = self.mutated(
            "libvmaf-build-matrix.yml",
            "    needs: tier\n    if: needs.tier.outputs.full == 'true'\n    name: ${{ matrix.name }}",
            "    name: ${{ matrix.name }}",
        )
        with self.assertRaises(AssertionError):
            expect_light_tier(contract, OWN)

    def test_a_light_job_gated_on_the_full_tier_is_caught(self) -> None:
        contract = self.mutated(
            "tests-and-quality-gates.yml",
            "needs.tier.outputs.light == 'true'",
            "needs.tier.outputs.full == 'true'",
        )
        with self.assertRaises(AssertionError):
            expect_light_tier(contract, OWN)

    def test_an_own_input_lane_gated_on_the_full_tier_is_caught(self) -> None:
        """Putting the Windows zip lane back behind the full tier is what ADR-2198 undoes."""
        contract = self.mutated(
            "windows-tester-bundle.yml",
            "    if: always() && needs.tier.outputs.light == 'true'\n",
            "    if: always() && needs.tier.outputs.full == 'true'\n",
        )
        with self.assertRaises(AssertionError):
            expect_own_input_lanes(contract, OWN)

    def test_an_own_input_planner_gated_on_the_full_tier_is_caught(self) -> None:
        contract = self.mutated(
            "windows-tester-bundle.yml",
            "    needs: tier\n    if: needs.tier.outputs.light == 'true'\n    name: Plan Windows zip impact",
            "    needs: tier\n    if: needs.tier.outputs.full == 'true'\n    name: Plan Windows zip impact",
        )
        with self.assertRaises(AssertionError):
            expect_own_input_lanes(contract, OWN)

    GPU_IF = (
        "    if: ${{ !cancelled() && (needs.tier.outputs.full == 'true' || "
        "needs.impact.result == 'failure' || (needs.impact.result == 'success' && "
        "needs.impact.outputs.msvc-sycl == 'true')) }}\n"
    )

    def test_an_msvc_lane_put_back_behind_the_full_tier_is_caught(self) -> None:
        """ADR-3279: the job gated on the full tier alone is the state before the lane."""
        contract = self.mutated(
            GPU_WORKFLOW, self.GPU_IF, "    if: needs.tier.outputs.full == 'true'\n"
        )
        with self.assertRaises(AssertionError):
            expect_own_input_lanes(contract, OWN)
        with self.assertRaises(AssertionError):
            expect_gpu_job_condition(contract)

    def test_an_msvc_planner_gated_on_the_full_tier_is_caught(self) -> None:
        contract = self.mutated(
            GPU_WORKFLOW,
            "    needs: tier\n    if: needs.tier.outputs.light == 'true'\n"
            "    name: Plan MSVC leg impact",
            "    needs: tier\n    if: needs.tier.outputs.full == 'true'\n"
            "    name: Plan MSVC leg impact",
        )
        with self.assertRaises(AssertionError):
            expect_own_input_lanes(contract, OWN)

    def test_an_msvc_lane_row_marked_full_is_caught(self) -> None:
        contract = self.mutated(
            GPU_WORKFLOW,
            "            tier: light\n            werror: msvc\n"
            "            name: Windows MSVC+SYCL\n",
            "            tier: full\n            werror: msvc\n"
            "            name: Windows MSVC+SYCL\n",
        )
        with self.assertRaises(AssertionError):
            expect_own_input_lanes(contract, OWN)
        with self.assertRaises(AssertionError):
            expect_gpu_rows_tiered(contract)

    def test_a_full_only_msvc_row_marked_light_is_caught(self) -> None:
        contract = self.mutated(
            GPU_WORKFLOW,
            "            tier: full\n            werror: msvc\n"
            "            name: Windows MSVC+CUDA\n",
            "            tier: light\n            werror: msvc\n"
            "            name: Windows MSVC+CUDA\n",
        )
        with self.assertRaises(AssertionError):
            expect_light_tier(contract, OWN)
        with self.assertRaises(AssertionError):
            expect_gpu_rows_tiered(contract)

    def test_an_msvc_step_that_ignores_the_leg_decision_is_caught(self) -> None:
        contract = self.mutated(
            GPU_WORKFLOW,
            "      - name: Install meson and ninja\n        if: steps.leg.outputs.run == 'true'\n"
            "        run: python -m pip install",
            "      - name: Install meson and ninja\n        run: python -m pip install",
        )
        with self.assertRaises(AssertionError):
            expect_gpu_steps_gated(contract)

    def test_an_msvc_checkout_behind_the_leg_decision_is_caught(self) -> None:
        """The checkout-ordering gate counts no checkout that carries an `if`."""
        contract = self.mutated(
            GPU_WORKFLOW,
            "      # `if`. A row the tier does not owe stops after this shallow checkout.\n"
            "      - uses: actions/checkout@",
            "      # `if`. A row the tier does not owe stops after this shallow checkout.\n"
            "      - if: steps.leg.outputs.run == 'true'\n"
            "        uses: actions/checkout@",
        )
        with self.assertRaises(AssertionError):
            expect_gpu_steps_gated(contract)

    def test_an_msvc_checkout_with_full_history_is_caught(self) -> None:
        """A row that does no work must not fetch the history."""
        contract = self.mutated(
            GPU_WORKFLOW,
            "          fetch-depth: 1\n\n      - name: Setup python\n"
            "        if: steps.leg.outputs.run == 'true'\n",
            "          fetch-depth: 0\n\n      - name: Setup python\n"
            "        if: steps.leg.outputs.run == 'true'\n",
        )
        with self.assertRaises(AssertionError):
            expect_gpu_steps_gated(contract)

    def test_an_msvc_leg_decision_that_runs_a_full_row_on_the_light_tier_is_caught(self) -> None:
        contract = self.mutated(
            GPU_WORKFLOW,
            "        shell: bash\n        env:\n          LEG_TIER: ${{ matrix.tier }}\n"
            "          TIER_FULL: ${{ needs.tier.outputs.full }}\n        run: |\n"
            '          if [ "$LEG_TIER" = light ] || [ "$TIER_FULL" = true ]; then\n',
            "        shell: bash\n        env:\n          LEG_TIER: ${{ matrix.tier }}\n"
            "          TIER_FULL: ${{ needs.tier.outputs.full }}\n        run: |\n"
            '          if [ "$LEG_TIER" != none ] || [ "$TIER_FULL" = true ]; then\n',
        )
        with self.assertRaises(AssertionError):
            expect_gpu_leg_decision(contract)

    def test_an_msvc_job_that_skips_on_a_failed_planner_is_caught(self) -> None:
        """Without the fail-closed term a planner error would leave the lane unrun and green."""
        contract = self.mutated(
            GPU_WORKFLOW,
            self.GPU_IF,
            "    if: ${{ !cancelled() && (needs.tier.outputs.full == 'true' || "
            "(needs.impact.result == 'success' && "
            "needs.impact.outputs.msvc-sycl == 'true')) }}\n",
        )
        with self.assertRaises(AssertionError):
            expect_gpu_job_condition(contract)

    def test_an_msvc_job_that_starts_in_a_cancelled_run_is_caught(self) -> None:
        contract = self.mutated(GPU_WORKFLOW, "    if: ${{ !cancelled() && (", "    if: ${{ (")
        with self.assertRaises(AssertionError):
            expect_gpu_job_condition(contract)

    def test_a_job_without_the_draft_gate_is_caught(self) -> None:
        contract = self.mutated(
            "helm-chart.yml",
            "  impact:\n    needs: tier\n    if: needs.tier.outputs.light == 'true'\n",
            "  impact:\n",
        )
        with self.assertRaises(AssertionError):
            expect_nothing_but_exceptions(contract, with_(OWN, draft=True))

    def test_a_praetor_step_that_runs_on_a_draft_is_caught(self) -> None:
        contract = self.mutated(
            "praetor-docs.yml",
            "        if: github.event.pull_request.draft != true\n        run: node tools/markdownlint/verify.mjs",
            "        run: node tools/markdownlint/verify.mjs",
        )
        with self.assertRaises(AssertionError):
            expect_draft_stop_before_any_work(contract, "praetor-docs.yml", "documentation")

    def test_a_praetor_job_without_the_draft_stop_is_caught(self) -> None:
        contract = self.mutated(
            "praetor-api.yml", "        if: github.event.pull_request.draft == true\n", ""
        )
        with self.assertRaises(AssertionError):
            expect_draft_stop_before_any_work(contract, "praetor-api.yml", "api-compatibility")

    def test_a_praetor_push_trigger_without_a_branch_filter_is_caught(self) -> None:
        contract = self.mutated("praetor-docs.yml", "    branches: ['master']\n", "")
        with self.assertRaises(AssertionError):
            expect_no_other_branch_runs(contract, FEATURE_PUSH)

    def test_an_unfiltered_push_trigger_is_caught(self) -> None:
        contract = self.mutated("docs.yml", "  push:\n    branches: [master]\n", "  push:\n")
        with self.assertRaises(AssertionError):
            expect_no_other_branch_runs(contract, FEATURE_PUSH)

    def test_a_workflow_that_listens_for_every_label_is_caught(self) -> None:
        contract = self.mutated(
            "docs.yml",
            "types: [opened, synchronize, reopened, ready_for_review]",
            "types: [opened, synchronize, reopened, ready_for_review, labeled]",
        )
        with self.assertRaises(AssertionError):
            expect_no_jobs(contract, with_(OWN, action="labeled", label="dependencies"))

    def test_a_release_job_that_ignores_the_release_tier_is_caught(self) -> None:
        contract = self.mutated(
            "lint-and-format.yml",
            "  pre-commit:\n    needs: tier\n    if: needs.tier.outputs.light == 'true'\n",
            "  pre-commit:\n    needs: tier\n    if: needs.tier.result == 'success'\n",
        )
        with self.assertRaises(AssertionError):
            expect_release_light(contract, RELEASE)


if __name__ == "__main__":
    unittest.main()
