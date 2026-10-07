#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Push-only and release-only workflows get a pull-request or scheduled verify (ADR-1595).

The tester image, Windows zip and macOS bundle workflows used to run only on a
push to master or a dispatch; the production, operator / server / node and
supply-chain workflows only on a published release. This test holds the
verification that now runs earlier:

* the `validate` job of each tester workflow is *executed* (its shell is cut out
  of the workflow and run in a scratch Git repository) for a pull-request, a push
  and a schedule event, and must resolve a source, never publish, and narrow the
  build to the legs the event pays for;
* `release-dry-run.yml` must stay a dry run: no credential, no push, no signing,
  every action pinned, every job routed by the plan;
* what the dry run builds must be what the release builds, so the two cannot
  drift apart unnoticed.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github" / "workflows"
GIT = shutil.which("git") or "/usr/bin/git"
BASH = shutil.which("bash") or "/bin/bash"
SHA_PIN = re.compile(r"^[^@\s]+@[0-9a-f]{40}$")


def load(name: str) -> dict[str, Any]:
    data: dict[str, Any] = yaml.safe_load((WORKFLOWS / name).read_text(encoding="utf-8"))
    return data


def triggers(wf: dict[str, Any]) -> dict[str, Any]:
    # YAML 1.1 reads the bare key `on` as the boolean True.
    found: dict[str, Any] = wf.get("on") or wf.get(True) or {}  # type: ignore[call-overload]
    return found


def validate_script(name: str) -> str:
    steps = load(name)["jobs"]["validate"]["steps"]
    return str(next(s["run"] for s in steps if s.get("id") == "src"))


def _git(env: dict[str, str], cwd: Path, *args: str) -> str:
    return subprocess.run(  # noqa: S603 -- fixed git argv, no shell
        [GIT, *args], cwd=cwd, env=env, check=True, capture_output=True, text=True
    ).stdout.strip()


def _scratch_clone(tmp_path: Path, env: dict[str, str]) -> tuple[Path, str]:
    """An origin with a tag and one commit after it, and a clone of it; the clone's HEAD."""
    origin = tmp_path / "origin"
    work = tmp_path / "work"
    origin.mkdir()
    _git(env, origin, "init", "-q", "-b", "master")
    (origin / "f").write_text("1")
    _git(env, origin, "add", "f")
    _git(env, origin, "commit", "-q", "-m", "one")
    _git(env, origin, "tag", "-a", "v1.0.0", "-m", "v1.0.0")
    (origin / "f").write_text("2")
    _git(env, origin, "commit", "-q", "-am", "two")
    _git(env, tmp_path, "clone", "-q", str(origin), str(work))
    return work, _git(env, work, "rev-parse", "HEAD")


def run_validate(
    name: str, event: str, ref: str, extra_env: dict[str, str] | None = None
) -> tuple[int, dict[str, str], str]:
    """Run the `validate` job's resolve step in a scratch clone; return its outputs."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        env_base = {
            "PATH": os.environ["PATH"],
            "HOME": tmp,
            "GIT_CONFIG_GLOBAL": "/dev/null",
            "GIT_AUTHOR_NAME": "t",
            "GIT_AUTHOR_EMAIL": "t@example.invalid",
            "GIT_COMMITTER_NAME": "t",
            "GIT_COMMITTER_EMAIL": "t@example.invalid",
        }
        work, sha = _scratch_clone(tmp_path, env_base)
        out = tmp_path / "output"
        out.touch()
        (tmp_path / "summary").touch()
        env = {
            **env_base,
            "GITHUB_EVENT_NAME": event,
            "GITHUB_REF": ref,
            "GITHUB_SHA": sha,
            "GITHUB_OUTPUT": str(out),
            "GITHUB_STEP_SUMMARY": str(tmp_path / "summary"),
            "GITHUB_REPOSITORY": "VMAFx/vmafx",
            "RUNNER_TEMP": str(tmp_path),
            "DEFAULT_BRANCH": "master",
            "GH_TOKEN": "unused",
            # Inputs of a dispatch, empty for every other event.
            "PUBLISH_TAG": "",
            "SOURCE_REF": "",
            "TAG": "",
            "REF": "",
            "PUBLISH_INPUT": "",
            # Planner outputs of the Windows zip workflow (ADR-2198).
            "SELECTED": "",
            "SELECTED_SYCL": "",
            **(extra_env or {}),
        }
        done = subprocess.run(  # noqa: S603 -- bash running the workflow step under test
            [BASH, "-eo", "pipefail", "-c", validate_script(name)],
            cwd=work,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        lines = out.read_text(encoding="utf-8").splitlines()
        return done.returncode, dict(ln.split("=", 1) for ln in lines if "=" in ln), done.stderr


# Text in a step that pushes, signs or attests something (ADR-1701 nightly contract).
PUBLISHING_MARKERS = ("login-action", "push=true", "push: true", "cosign", "attest", "sbom")


def cron_minutes(cron: str) -> int:
    """Minute of the day a `M H * * *`-shaped cron line starts at."""
    minute, hour = cron.split()[:2]
    return int(hour) * 60 + int(minute)


def legs(matrix_json: str, key: str) -> list[str]:
    return sorted(leg[key] for leg in json.loads(matrix_json)["include"])


def assert_routed_in_job(case: unittest.TestCase, name: str, selector: str) -> None:
    """ADR-1687: the workflow always starts and the impact planner routes the build."""
    wf = load(name)
    on = triggers(wf)
    case.assertEqual(on["pull_request"]["branches"], ["master"])
    case.assertEqual(on["push"]["branches"], ["master"])
    for event in ("pull_request", "push"):
        case.assertNotIn("paths", on[event])
    case.assertEqual(
        wf["jobs"]["impact"]["outputs"]["selected"], f"${{{{ steps.impact.outputs.{selector} }}}}"
    )
    case.assertEqual(wf["jobs"]["validate"]["needs"], "impact")
    case.assertIn("needs.impact.outputs.selected == 'true'", wf["jobs"]["validate"]["if"])


class TesterImage(unittest.TestCase):
    NAME = "docker-publish-tester.yml"

    def test_every_pull_request_starts_and_the_planner_routes_the_build(self) -> None:
        assert_routed_in_job(self, self.NAME, "tester_image")

    def test_pull_request_builds_amd64_only_and_never_publishes(self) -> None:
        rc, out, err = run_validate(self.NAME, "pull_request", "refs/pull/7/merge")
        self.assertEqual(rc, 0, err)
        self.assertEqual(out["publish"], "false")
        self.assertEqual(legs(out["matrix"], "arch"), ["amd64"])

    def test_push_builds_both_architectures_and_never_publishes(self) -> None:
        rc, out, err = run_validate(self.NAME, "push", "refs/heads/master")
        self.assertEqual(rc, 0, err)
        self.assertEqual(out["publish"], "false")
        self.assertEqual(legs(out["matrix"], "arch"), ["amd64", "arm64"])

    def test_build_job_takes_its_matrix_from_validate(self) -> None:
        build = load(self.NAME)["jobs"]["build"]
        self.assertIn("needs.validate.outputs.matrix", build["strategy"]["matrix"])

    def test_gpu_images_and_publishing_are_off_on_a_pull_request(self) -> None:
        jobs = load(self.NAME)["jobs"]
        self.assertIn("pull_request", jobs["build-gpu"]["if"])
        for name in ("publish", "publish-gpu"):
            self.assertIn("publish == 'true'", jobs[name]["if"])

    def test_a_pull_request_run_is_cancelled_by_a_newer_one_only(self) -> None:
        wf = load(self.NAME)
        self.assertIn(
            "github.event_name == 'pull_request'", wf["concurrency"]["cancel-in-progress"]
        )
        self.assertIn("pull_request.number", wf["concurrency"]["group"])

    def test_nightly_schedule_builds_amd64_from_master_and_never_publishes(self) -> None:
        # ADR-1701: the nightly build and test of master's head.
        wf = load(self.NAME)
        schedule = triggers(wf)["schedule"]
        self.assertEqual(len(schedule), 1)
        self.assertEqual(schedule[0]["cron"].split()[2:], ["*", "*", "*"])  # every night
        self.assertIn("'schedule'", wf["concurrency"]["group"])  # a group of its own
        rc, out, err = run_validate(self.NAME, "schedule", "refs/heads/master")
        self.assertEqual(rc, 0, err)
        self.assertEqual(out["publish"], "false")
        self.assertEqual(legs(out["matrix"], "arch"), ["amd64"])

    def test_no_job_a_scheduled_run_reaches_pushes_signs_or_attests(self) -> None:
        jobs = load(self.NAME)["jobs"]
        self.assertIn("github.event_name != 'schedule'", jobs["build-gpu"]["if"])
        reached = {
            name: job
            for name, job in jobs.items()
            if "publish == 'true'" not in str(job.get("if", ""))
            and "!= 'schedule'" not in str(job.get("if", ""))
        }
        self.assertEqual(
            set(reached), {"tier", "impact", "validate", "refs-x86", "build", "tester-image"}
        )
        for name, job in reached.items():
            for step in job.get("steps", []):
                text = json.dumps(step)
                if any(marker in text for marker in PUBLISHING_MARKERS):
                    with self.subTest(job=name, step=step.get("name", step.get("uses"))):
                        self.assertIn("publish == 'true'", str(step.get("if", "")))

    def test_nightly_ends_before_the_nightly_jobs_and_the_release_dry_run(self) -> None:
        jobs = load(self.NAME)["jobs"]
        start = cron_minutes(triggers(load(self.NAME))["schedule"][0]["cron"])
        chain = ("impact", "validate", "refs-x86", "build", "tester-image")
        latest_end = start + sum(int(jobs[name]["timeout-minutes"]) for name in chain)
        for other in ("nightly.yml", "release-dry-run.yml"):
            with self.subTest(workflow=other):
                cron = triggers(load(other))["schedule"][0]["cron"]
                self.assertLess(latest_end, cron_minutes(cron), cron)


class WindowsBundle(unittest.TestCase):
    NAME = "windows-tester-bundle.yml"

    def test_every_pull_request_starts_and_the_planner_routes_the_build(self) -> None:
        assert_routed_in_job(self, self.NAME, "windows_tester_zip")

    def test_pull_request_builds_what_the_planner_selected_and_never_publishes(self) -> None:
        cases = (
            ({"SELECTED": "true", "SELECTED_SYCL": "true"}, ["x64", "x64-sycl"]),
            ({"SELECTED": "false", "SELECTED_SYCL": "true"}, ["x64-sycl"]),
            ({"SELECTED": "true", "SELECTED_SYCL": "false"}, ["x64"]),
        )
        for env, expected in cases:
            with self.subTest(env=env):
                rc, out, err = run_validate(self.NAME, "pull_request", "refs/pull/7/merge", env)
                self.assertEqual(rc, 0, err)
                self.assertEqual(out["publish"], "false")
                self.assertEqual(sorted(legs(out["build_matrix"], "name")), expected)
                self.assertEqual(sorted(json.loads(out["verify_matrix"])["name"]), expected)

    def test_pull_request_never_builds_arm64_or_cuda(self) -> None:
        env = {"SELECTED": "true", "SELECTED_SYCL": "true"}
        _, out, _ = run_validate(self.NAME, "pull_request", "refs/pull/7/merge", env)
        self.assertNotIn("arm64", legs(out["build_matrix"], "name"))
        self.assertNotIn("x64-cuda", legs(out["build_matrix"], "name"))

    def test_the_validate_job_starts_for_either_selector(self) -> None:
        condition = load(self.NAME)["jobs"]["validate"]["if"]
        self.assertIn("needs.impact.outputs.selected == 'true'", condition)
        self.assertIn("needs.impact.outputs.selected-sycl == 'true'", condition)

    def test_push_builds_every_zip(self) -> None:
        rc, out, err = run_validate(self.NAME, "push", "refs/heads/master")
        self.assertEqual(rc, 0, err)
        self.assertEqual(out["publish"], "false")
        self.assertEqual(
            legs(out["build_matrix"], "name"), ["arm64", "x64", "x64-cuda", "x64-sycl"]
        )
        self.assertEqual(
            sorted(json.loads(out["verify_matrix"])["name"]),
            ["arm64", "x64", "x64-cuda", "x64-sycl"],
        )

    def test_every_other_ref_is_still_refused(self) -> None:
        rc, _, _ = run_validate(self.NAME, "push", "refs/heads/feature")
        self.assertNotEqual(rc, 0)

    def test_jobs_take_their_matrices_from_validate(self) -> None:
        jobs = load(self.NAME)["jobs"]
        self.assertIn("outputs.build-matrix", jobs["build"]["strategy"]["matrix"])
        self.assertIn("outputs.verify-matrix", jobs["verify"]["strategy"]["matrix"])


class MacosBundle(unittest.TestCase):
    NAME = "macos-tester-bundle.yml"

    def test_has_a_weekly_schedule(self) -> None:
        on = triggers(load(self.NAME))
        self.assertEqual(len(on["schedule"]), 1)
        fields = on["schedule"][0]["cron"].split()
        self.assertEqual(len(fields), 5)
        self.assertEqual(fields[2:4], ["*", "*"])  # every month, any day of month
        self.assertRegex(fields[4], r"^[0-6]$")  # one weekday: weekly

    def test_a_scheduled_run_needs_no_inputs_and_never_publishes(self) -> None:
        rc, out, err = run_validate(self.NAME, "schedule", "refs/heads/master")
        self.assertEqual(rc, 0, err)
        self.assertRegex(out["source_sha"], r"^[0-9a-f]{40}$")
        self.assertEqual(out["describe"], "v1.0.0-1-g" + out["source_sha"][:7])
        publish = load(self.NAME)["jobs"]["publish"]["if"]
        self.assertIn("inputs.publish == 'true'", publish)

    def test_a_dispatch_without_an_input_is_still_refused(self) -> None:
        rc, _, err = run_validate(self.NAME, "workflow_dispatch", "refs/heads/master")
        self.assertNotEqual(rc, 0)
        self.assertIn("exactly one of the inputs ref and tag", err)


def release_build_targets() -> set[tuple[str, str]]:
    """(Dockerfile, target) of every image build the release workflows run."""
    found: set[tuple[str, str]] = set()
    for name in ("docker-publish-production.yml", "docker-publish-operator-node.yml"):
        for job in load(name)["jobs"].values():
            for step in job.get("steps", []):
                with_ = step.get("with", {})
                if "build-push-action" in step.get("uses", "") and "target" in with_:
                    found.add((with_["file"], with_["target"]))
    return found


class ReleaseDryRun(unittest.TestCase):
    NAME = "release-dry-run.yml"

    def setUp(self) -> None:
        self.wf = load(self.NAME)
        self.text = (WORKFLOWS / self.NAME).read_text(encoding="utf-8")

    def test_runs_on_pull_requests_and_weekly(self) -> None:
        on = triggers(self.wf)
        self.assertEqual(on["pull_request"]["branches"], ["master"])
        self.assertNotIn("paths", on["pull_request"])  # routed in-job, ADR-1140
        self.assertEqual(len(on["schedule"]), 1)
        self.assertIn("workflow_dispatch", on)
        self.assertNotIn("push", on)
        self.assertNotIn("release", on)

    def test_is_a_dry_run(self) -> None:
        self.assertEqual(self.wf["permissions"], {"contents": "read"})
        for name, job in self.wf["jobs"].items():
            # ADR-2169: the tier call reads the pull request's labels and nothing more.
            expected = (
                {"contents": "read", "pull-requests": "read"}
                if name == "tier"
                else {"contents": "read"}
            )
            self.assertEqual(job.get("permissions"), expected, name)
            self.assertNotIn("environment", job)
        for forbidden in (
            "docker/login-action",
            "cosign",
            "attest-build-provenance",
            "pypi-publish",
            "secrets.",
            "id-token",
            "packages: write",
            "push: true",
            "gh release",
        ):
            self.assertNotIn(forbidden, self.text)

    def test_every_action_is_pinned_to_a_commit(self) -> None:
        for job in self.wf["jobs"].values():
            for step in job.get("steps", []):  # the tier call (ADR-2169) is a local workflow
                if "uses" in step:
                    self.assertRegex(step["uses"], SHA_PIN)

    def test_every_job_but_the_plan_and_the_gate_is_routed_by_it(self) -> None:
        for name, job in self.wf["jobs"].items():
            if name in {"plan", "gate", "tier"}:
                continue
            self.assertEqual(job["needs"], "plan", name)
            self.assertRegex(job["if"], r"needs\.plan\.outputs\.\w+ == 'true'", name)
        # ADR-1687: the required context waits for every routed job.
        gate = self.wf["jobs"]["gate"]
        self.assertEqual(gate["name"], "Release Dry Run")
        self.assertEqual(gate["if"], "always() && needs.tier.outputs.full == 'true'")
        self.assertEqual(set(gate["needs"]), set(self.wf["jobs"]) - {"gate"})

    def test_the_images_it_builds_are_the_images_the_release_builds(self) -> None:
        release = release_build_targets()
        images = self.wf["jobs"]["images"]["strategy"]["matrix"]["include"]
        for leg in images:
            self.assertIn((leg["file"], leg["target"]), release, leg["name"])
        gpu = self.wf["jobs"]["gpu"]["strategy"]["matrix"]["include"]
        for leg in gpu:
            self.assertIn(("docker/Dockerfile.production-gpu", leg["target"]), release, leg["name"])
        self.assertEqual(
            {leg["target"] for leg in gpu},
            {
                t
                for f, t in release
                if f == "docker/Dockerfile.production-gpu" and t.startswith("final-")
            },
        )

    def test_mcp_commands_and_pins_are_the_releases(self) -> None:
        supply = (WORKFLOWS / "supply-chain.yml").read_text(encoding="utf-8")
        for needle in (
            "python -m build --no-isolation --wheel --sdist --outdir ../../mcp-dist",
            "requirements/locks/package-build.txt",
            "mcp-server/vmaf-mcp/requirements-runtime-lock.txt",
            "scripts/release/verify-mcp-sbom.sh",
        ):
            self.assertIn(needle, supply, needle)
            self.assertIn(needle, self.text, needle)
        syft = r"syft-version: (\S+)"
        self.assertEqual(set(re.findall(syft, supply)), set(re.findall(syft, self.text)))
        action = r"anchore/sbom-action@[0-9a-f]{40}"
        self.assertEqual(set(re.findall(action, supply)), set(re.findall(action, self.text)))

    def test_release_sbom_job_uses_the_shared_verifier(self) -> None:
        sbom_steps = load("supply-chain.yml")["jobs"]["sbom"]["steps"]
        runs = "\n".join(s.get("run", "") for s in sbom_steps)
        self.assertIn("scripts/release/verify-mcp-sbom.sh", runs)
        self.assertNotIn("pkg:pypi/vmaf-mcp@", runs)

    def test_plan_script_names_only_files_that_exist(self) -> None:
        text = (ROOT / "scripts/ci/release-dry-run-plan.sh").read_text(encoding="utf-8")
        code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
        arms = re.findall(r"case \"\$path\" in\n(.*?)\n\s+esac", code, re.S)
        tokens = set()
        for arm in arms:
            tokens.update(re.findall(r"[A-Za-z0-9_.][A-Za-z0-9_./-]*\*?", arm.replace("\\", " ")))
        tokens = {t for t in tokens if "/" in t or t.endswith("*") or "." in t}
        self.assertGreater(len(tokens), 25, len(tokens))
        for token in sorted(tokens):
            if token.endswith("*"):
                self.assertTrue(list(ROOT.glob(token)), f"{token} matches nothing")
            else:
                self.assertTrue((ROOT / token).exists(), f"{token} does not exist")


if __name__ == "__main__":
    unittest.main()
