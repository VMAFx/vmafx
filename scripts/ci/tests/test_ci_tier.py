#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for scripts/ci/ci_tier.py (ADR-2169, ADR-3536)."""

from __future__ import annotations

import http.client
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
import urllib.error
import urllib.request
from collections.abc import Sequence
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import ci_tier
from scripts.lib.safe_subprocess import run as run_command

CONFIG = ci_tier.load_config()
REPO = "VMAFx/vmafx"
RELEASE_REF = "release-please--branches--master--components--vmafx"
PR_3401_PATHS = (
    ".github/workflows/libvmaf-build-matrix.yml",
    "core/test/test_cuda_parity_gate_skip.py",
    "docs/adr/3532-trusted-executable-launch-links.md",
    "docs/adr/_index_fragments/3532-trusted-executable-launch-links.md",
    "scripts/lib/AGENTS.md",
    "scripts/lib/safe_subprocess.py",
    "scripts/lib/test_safe_subprocess.py",
    "tools/rc1-tester/src/vmaf_rc1_tester/_safe_subprocess.py",
)


def event(**changes: object) -> ci_tier.Event:
    base = {
        "name": "pull_request",
        "repository": REPO,
        "head_repository": REPO,
        "head_ref": "fix/example",
        "author": "someone",
        "author_type": "User",
        "labels": (),
    }
    return ci_tier.Event(**{**base, **changes})  # type: ignore[arg-type]


def decide(
    facts: ci_tier.Event,
    *,
    exempt: bool = False,
    paths: Sequence[str] | None = ("docs/usage/cli.md",),
) -> ci_tier.Decision:
    """Decide with ``CONFIG`` and deterministic callables."""
    return ci_tier.decide(
        facts,
        CONFIG,
        release_exempt=lambda: exempt,
        changed_paths=lambda: paths,
    )


class Decide(unittest.TestCase):
    def test_not_a_pull_request_is_full(self) -> None:
        def refuse_paths() -> Sequence[str]:
            raise AssertionError("changed_paths asked for a non-pull-request event")

        for name in ("push", "schedule", "workflow_dispatch", "release"):
            decision = ci_tier.decide(
                event(name=name),
                CONFIG,
                release_exempt=lambda: False,
                changed_paths=refuse_paths,
            )
            self.assertEqual(decision.tier, ci_tier.FULL, name)
            self.assertEqual(decision.reason, f"event {name} is not a pull request")

    def test_own_pull_request_is_light(self) -> None:
        self.assertEqual(decide(event(), paths=("docs/usage/cli.md",)).tier, ci_tier.LIGHT)

    def test_planted_pr_3401_paths_escalate_own_pull_request_to_full(self) -> None:
        decision = decide(event(), paths=PR_3401_PATHS)
        self.assertEqual(decision.tier, ci_tier.FULL)
        self.assertEqual(
            decision.reason,
            "changed path 'scripts/lib/safe_subprocess.py' matches full_paths "
            "'scripts/lib/safe_subprocess.py'",
        )

    def test_each_seeded_full_path_alone_escalates_own_pull_request_to_full(self) -> None:
        for listed in CONFIG["full_paths"]:
            with self.subTest(path=listed):
                decision = decide(event(), paths=(listed,))
                self.assertEqual(decision.tier, ci_tier.FULL)
                self.assertIn(listed, decision.reason)

    def test_fork_pull_request_is_full(self) -> None:
        def refuse_paths() -> Sequence[str]:
            raise AssertionError("changed_paths asked for a fork pull request")

        decision = ci_tier.decide(
            event(head_repository="x/vmafx"),
            CONFIG,
            release_exempt=lambda: False,
            changed_paths=refuse_paths,
        )
        self.assertEqual(decision.tier, ci_tier.FULL)
        self.assertEqual(decision.reason, "fork pull request (head repository 'x/vmafx')")

    def test_missing_head_repository_is_full(self) -> None:
        """A payload without a head repository (a deleted fork) is not trusted as own."""
        self.assertEqual(decide(event(head_repository="")).tier, ci_tier.FULL)

    def test_full_label_is_full(self) -> None:
        def refuse_paths() -> Sequence[str]:
            raise AssertionError("changed_paths asked when ci: full label is set")

        decision = ci_tier.decide(
            event(labels=("ci: full",)),
            CONFIG,
            release_exempt=lambda: False,
            changed_paths=refuse_paths,
        )
        self.assertEqual(decision.tier, ci_tier.FULL)
        self.assertEqual(decision.reason, "label 'ci: full'")

    def test_release_branch_needs_the_exemption_to_be_light(self) -> None:
        facts = event(head_ref=RELEASE_REF)
        self.assertEqual(decide(facts, exempt=True).tier, ci_tier.RELEASE_LIGHT)
        self.assertEqual(decide(facts, exempt=False).tier, ci_tier.LIGHT)

    def test_cut_label_makes_the_release_pull_request_full(self) -> None:
        facts = event(head_ref=RELEASE_REF, labels=("autorelease: cut",))
        self.assertEqual(decide(facts, exempt=True).tier, ci_tier.FULL)

    def test_cut_label_elsewhere_changes_nothing(self) -> None:
        self.assertEqual(decide(event(labels=("autorelease: cut",))).tier, ci_tier.LIGHT)

    def test_exemption_is_asked_only_for_a_release_branch(self) -> None:
        def refuse() -> bool:
            raise AssertionError("asked for an ordinary pull request")

        decision = ci_tier.decide(
            event(),
            CONFIG,
            release_exempt=refuse,
            changed_paths=lambda: ("docs/usage/cli.md",),
        )
        self.assertEqual(decision.tier, "light")

    def test_glob_boundaries_respect_fnmatchcase_semantics(self) -> None:
        for non_match in (
            "scripts/lib/safe_subprocess.py.orig",
            "scripts/lib/Safe_Subprocess.py",
            "other/scripts/lib/safe_subprocess.py",
        ):
            with self.subTest(path=non_match):
                self.assertEqual(decide(event(), paths=(non_match,)).tier, ci_tier.LIGHT)
        wildcard_cfg = {**CONFIG, "full_paths": ["scripts/*/safe_subprocess.py"]}
        decision = ci_tier.decide(
            event(),
            wildcard_cfg,
            release_exempt=lambda: False,
            changed_paths=lambda: ("scripts/lib/safe_subprocess.py",),
        )
        self.assertEqual(decision.tier, ci_tier.FULL)
        self.assertIn("scripts/lib/safe_subprocess.py", decision.reason)
        self.assertIn("scripts/*/safe_subprocess.py", decision.reason)

    def test_empty_full_paths_stays_light_without_fetching_paths(self) -> None:
        def refuse_paths() -> Sequence[str]:
            raise AssertionError("changed_paths called when full_paths is empty")

        empty_cfg = {**CONFIG, "full_paths": []}
        decision = ci_tier.decide(
            event(),
            empty_cfg,
            release_exempt=lambda: False,
            changed_paths=refuse_paths,
        )
        self.assertEqual(decision.tier, ci_tier.LIGHT)

    def test_unavailable_changed_paths_fail_closed_to_full(self) -> None:
        decision = decide(event(), paths=None)
        self.assertEqual(decision.tier, ci_tier.FULL)
        self.assertEqual(decision.reason, "changed paths unavailable")

    def test_outputs_are_derived_from_the_tier(self) -> None:
        light = decide(event()).outputs()
        self.assertEqual((light["light"], light["full"]), ("true", "false"))
        full = decide(event(name="push")).outputs()
        self.assertEqual((full["light"], full["full"]), ("true", "true"))
        release = decide(event(head_ref=RELEASE_REF), exempt=True).outputs()
        self.assertEqual((release["light"], release["full"]), ("false", "false"))


class ReleaseExemption(unittest.TestCase):
    """The real release-pr-exempt.sh decides who the release pull request is."""

    def test_bot_author_on_a_release_branch_is_exempt(self) -> None:
        facts = event(head_ref=RELEASE_REF, author="github-actions[bot]", author_type="Bot")
        self.assertTrue(ci_tier.release_exempt_from_script(facts, ["x"]))

    def test_a_person_on_a_release_branch_is_not(self) -> None:
        facts = event(head_ref=RELEASE_REF, author="someone")
        self.assertFalse(
            ci_tier.release_exempt_from_script(facts, [".release-please-manifest.json"])
        )

    def test_the_maintainer_account_is_exempt_only_for_a_release_diff(self) -> None:
        facts = event(head_ref=RELEASE_REF, author="lusoris")
        self.assertTrue(
            ci_tier.release_exempt_from_script(facts, [".release-please-manifest.json"])
        )
        self.assertFalse(ci_tier.release_exempt_from_script(facts, ["core/src/feature/ssim.c"]))


class LiveLabels(unittest.TestCase):
    def test_labels_are_read_from_the_api_with_paging(self) -> None:
        pages = {
            1: [{"name": f"l{i}"} for i in range(ci_tier.PAGE_SIZE)],
            2: [{"name": "ci: full"}],
        }

        def fetch(path: str) -> object:
            return pages[int(path.rsplit("page=", 1)[1])]

        labels = ci_tier.live_labels(fetch, REPO, "7")
        self.assertEqual(len(labels), ci_tier.PAGE_SIZE + 1)
        self.assertIn("ci: full", labels)

    def test_a_non_list_answer_is_refused(self) -> None:
        with self.assertRaises(ci_tier.TierError):
            ci_tier.live_labels(lambda path: {"message": "Not Found"}, REPO, "7")

    def test_live_labels_beat_the_payload(self) -> None:
        env = {
            "EVENT_NAME": "pull_request",
            "GITHUB_REPOSITORY": REPO,
            "HEAD_REPOSITORY": REPO,
            "HEAD_REF": "fix/example",
            "PR_NUMBER": "7",
            "PR_LABELS": "[]",
        }
        facts = ci_tier.event_from_environment(env, lambda path: [{"name": "ci: full"}])
        self.assertEqual(facts.labels, ("ci: full",))

    def test_payload_labels_are_used_without_a_token_and_say_so(self) -> None:
        env = {"EVENT_NAME": "pull_request", "PR_NUMBER": "7", "PR_LABELS": '["a"]'}
        err = io.StringIO()
        with redirect_stderr(err):
            facts = ci_tier.event_from_environment(env, None)
        self.assertEqual(facts.labels, ("a",))
        self.assertIn("event payload", err.getvalue())

    def test_a_null_payload_means_no_labels(self) -> None:
        facts = ci_tier.event_from_environment({"EVENT_NAME": "push", "PR_LABELS": "null"}, None)
        self.assertEqual(facts.labels, ())


class Api(unittest.TestCase):
    """``api_request`` sends GitHub API calls over the https-only opener."""

    def test_a_non_https_url_is_refused(self) -> None:
        for url in ("http://api.github.com/x", "file:///etc/passwd", "ftp://h/x"):
            with self.subTest(url=url), self.assertRaises(ci_tier.TierError):
                ci_tier.api_request("GET", url, "t")

    def sent(
        self, method: str, payload: object | None = None, answer: bytes = b"{}"
    ) -> tuple[urllib.request.Request, Any]:
        """The request api_request hands to the https opener, and what it returned."""
        opened = mock.MagicMock()
        opened.__enter__.return_value.read.return_value = answer
        with mock.patch.object(ci_tier.HTTPS_OPENER, "open", return_value=opened) as sent:
            result = ci_tier.api_request(method, "https://api.github.com/x", "t", payload)
        return sent.call_args.args[0], result

    def test_a_payload_goes_out_as_a_json_body(self) -> None:
        """A payload goes out as a JSON body with its content type and method."""
        request, _ = self.sent("PATCH", {"sha": "c1", "force": False})
        data = request.data
        if not isinstance(data, bytes):
            self.fail(f"request body is {type(data).__name__}, not bytes")
        self.assertEqual(json.loads(data), {"sha": "c1", "force": False})
        self.assertEqual(request.get_header("Content-type"), "application/json")
        self.assertEqual(request.get_method(), "PATCH")

    def test_a_post_without_payload_sends_an_empty_body(self) -> None:
        """A POST without payload sends an empty body and no content type."""
        request, _ = self.sent("POST")
        self.assertEqual(request.data, b"")
        self.assertIsNone(request.get_header("Content-type"))

    def test_a_get_sends_no_body(self) -> None:
        """A GET sends no body and carries the bearer token."""
        request, _ = self.sent("GET")
        self.assertIsNone(request.data)
        self.assertEqual(request.get_header("Authorization"), "Bearer t")

    def test_an_empty_answer_reads_as_an_empty_object(self) -> None:
        """An empty answer reads as an empty object."""
        self.assertEqual(self.sent("GET", answer=b"")[1], {})


class HttpsJson(unittest.TestCase):
    """``https_json`` sends the caller's headers and bounds the answer size."""

    def answer(
        self, body: bytes, limit: int, headers: dict[str, str] | None = None
    ) -> tuple[Any, Any, Any]:
        """https_json's result for ``body`` and the request and read size it used."""
        opened = mock.MagicMock()
        opened.__enter__.return_value.read.return_value = body
        with mock.patch.object(ci_tier.HTTPS_OPENER, "open", return_value=opened) as sent:
            result = ci_tier.https_json("GET", "https://h/x", headers or {}, None, 5, limit)
        read = opened.__enter__.return_value.read
        return result, sent.call_args, read.call_args

    def test_an_answer_of_exactly_the_limit_is_read(self) -> None:
        """An answer of exactly the limit is read, with one byte more requested."""
        result, opened, read = self.answer(b'{"a":1}', 7)
        self.assertEqual(result, {"a": 1})
        self.assertEqual(read.args, (8,))
        self.assertEqual(opened.kwargs["timeout"], 5)

    def test_an_answer_one_byte_over_the_limit_is_refused(self) -> None:
        """An answer one byte over the limit is refused."""
        with self.assertRaisesRegex(ci_tier.TierError, "larger than 6 bytes"):
            self.answer(b'{"a":1}', 6)

    def test_the_caller_headers_go_out_and_nothing_else(self) -> None:
        """Only the caller's headers go out; no Authorization is added."""
        _, opened, _ = self.answer(b"{}", 10, {"Accept": "application/json"})
        request = opened.args[0]
        self.assertEqual(request.get_header("Accept"), "application/json")
        self.assertIsNone(request.get_header("Authorization"))

    def test_a_broken_answer_is_a_tier_error(self) -> None:
        """A cut-off body or a broken status line on open or read is a TierError."""
        for where, error in (
            ("read", http.client.IncompleteRead(b"par")),
            ("read", http.client.LineTooLong("header line")),
            ("open", http.client.BadStatusLine("x")),
            ("open", http.client.RemoteDisconnected("closed")),
        ):
            with self.subTest(where=where, error=type(error).__name__):
                opened = mock.MagicMock()
                opened.__enter__.return_value.read.side_effect = error
                kind = {"return_value": opened} if where == "read" else {"side_effect": error}
                with (
                    mock.patch.object(ci_tier.HTTPS_OPENER, "open", **kind),
                    self.assertRaisesRegex(ci_tier.TierError, type(error).__name__),
                ):
                    ci_tier.https_json("GET", "https://h/x", {}, None, 5, 10)

    def test_an_error_status_stays_an_http_error(self) -> None:
        """An error status is not turned into a TierError; callers read its code."""
        error = urllib.error.HTTPError("https://h/x", 404, "Not Found", None, None)  # type: ignore[arg-type]
        with (
            mock.patch.object(ci_tier.HTTPS_OPENER, "open", side_effect=error),
            self.assertRaises(urllib.error.HTTPError) as raised,
        ):
            ci_tier.https_json("GET", "https://h/x", {}, None, 5, 10)
        self.assertEqual(raised.exception.code, 404)
        raised.exception.close()

    def test_a_non_https_url_is_refused_before_any_request(self) -> None:
        """A non-https URL is refused before the opener is called."""
        with mock.patch.object(ci_tier.HTTPS_OPENER, "open") as sent:
            for url in ("http://h/x", "file:///etc/passwd"):
                with self.subTest(url=url), self.assertRaises(ci_tier.TierError):
                    ci_tier.https_json("GET", url, {}, None, 5, 10)
        sent.assert_not_called()


class HttpsOpener(unittest.TestCase):
    """The shared opener speaks https only and follows no redirect."""

    @staticmethod
    def response(code: int, location: str | None = None) -> mock.Mock:
        """A stand-in for an https answer with ``code`` and an optional Location."""
        headers = http.client.HTTPMessage()
        if location is not None:
            headers["Location"] = location
        answer = mock.Mock(code=code, msg="status")
        answer.info.return_value = headers
        return answer

    def test_an_https_address_reaches_the_https_handler(self) -> None:
        """An https address reaches the https handler unchanged."""
        answer = self.response(200)
        with mock.patch.object(
            urllib.request.HTTPSHandler, "https_open", return_value=answer
        ) as https_open:
            self.assertIs(ci_tier.HTTPS_OPENER.open("https://h/x", timeout=1), answer)
        self.assertEqual(https_open.call_args.args[0].full_url, "https://h/x")

    def test_a_non_https_address_fails_without_being_opened(self) -> None:
        """Every other scheme fails as an unknown url type."""
        for url in ("file:///etc/passwd", "data:text/plain,x", "ftp://h/x", "http://h/x"):
            with (
                self.subTest(url=url),
                self.assertRaisesRegex(urllib.error.URLError, "unknown url type"),
            ):
                ci_tier.HTTPS_OPENER.open(url, timeout=1)

    def test_a_redirect_is_not_followed(self) -> None:
        """Every 3xx answer is an HTTPError after exactly one request."""
        # An https target too: following it would resend the Authorization header.
        targets = ("https://other.example/y", "http://h/y", "ftp://h/y", "file:///etc/passwd")
        for code in (301, 302, 307, 308):
            for target in targets:
                with (
                    self.subTest(code=code, target=target),
                    mock.patch.object(
                        urllib.request.HTTPSHandler,
                        "https_open",
                        return_value=self.response(code, target),
                    ) as https_open,
                ):
                    with self.assertRaises(urllib.error.HTTPError) as raised:
                        ci_tier.HTTPS_OPENER.open("https://h/x", timeout=1)
                    raised.exception.close()
                    self.assertEqual(raised.exception.code, code)
                    self.assertEqual(https_open.call_count, 1)

    def test_an_error_answer_is_an_http_error(self) -> None:
        """A 404 answer is an HTTPError."""
        with (
            mock.patch.object(
                urllib.request.HTTPSHandler, "https_open", return_value=self.response(404)
            ),
            self.assertRaises(urllib.error.HTTPError) as raised,
        ):
            ci_tier.HTTPS_OPENER.open("https://h/x", timeout=1)
        raised.exception.close()
        self.assertEqual(raised.exception.code, 404)


class Config(unittest.TestCase):
    def write(self, text: str) -> Path:
        directory = tempfile.mkdtemp()
        path = Path(directory) / "ci-tier.json"
        path.write_text(text, encoding="utf-8")
        self.addCleanup(path.unlink)
        return path

    def test_a_name_in_both_lists_is_refused(self) -> None:
        config = {**CONFIG, "always": ["A"], "full_only": ["A"]}
        with self.assertRaises(ci_tier.TierError):
            ci_tier.load_config(self.write(json.dumps(config)))

    def test_a_missing_key_is_refused(self) -> None:
        with self.assertRaises(ci_tier.TierError):
            ci_tier.load_config(self.write('{"labels": {}}'))

    def test_an_own_input_lane_must_name_a_full_only_context(self) -> None:
        lane = {
            "workflow": "w.yml",
            "context": "Not Full Only",
            "selectors": ["s"],
            "reason": "r",
        }
        config = {**CONFIG, "own_input_lanes": [lane]}
        with self.assertRaises(ci_tier.TierError):
            ci_tier.load_config(self.write(json.dumps(config)))

    def test_an_own_input_lane_needs_selectors_and_a_reason(self) -> None:
        context = CONFIG["full_only"][0]
        for broken in (
            {"workflow": "w.yml", "context": context, "selectors": [], "reason": "r"},
            {"workflow": "w.yml", "context": context, "selectors": ["s"], "reason": " "},
            {"context": context, "selectors": ["s"], "reason": "r"},
        ):
            with self.subTest(lane=broken):
                config = {**CONFIG, "own_input_lanes": [broken]}
                with self.assertRaises(ci_tier.TierError):
                    ci_tier.load_config(self.write(json.dumps(config)))

    def test_the_declared_own_input_lanes_load(self) -> None:
        loaded = ci_tier.load_config(ci_tier.DEFAULT_CONFIG)
        self.assertTrue(loaded["own_input_lanes"])

    def test_malformed_full_paths_entries_are_refused(self) -> None:
        missing = {k: v for k, v in CONFIG.items() if k != "full_paths"}
        with self.assertRaises(ci_tier.TierError):
            ci_tier.load_config(self.write(json.dumps(missing)))
        invalid: object
        for invalid in (
            "scripts/lib/safe_subprocess.py",
            123,
            {},
            [""],
            ["   "],
            [123],
            ["scripts/lib/safe_subprocess.py", "scripts/lib/safe_subprocess.py"],
        ):
            with self.subTest(full_paths=invalid):
                config = {**CONFIG, "full_paths": invalid}
                with self.assertRaises(ci_tier.TierError):
                    ci_tier.load_config(self.write(json.dumps(config)))


class RunFullPaths(unittest.TestCase):
    """End-to-end ``run()`` coverage for lazy path fetching and fail-closed errors."""

    def base_env(self, **extra: str) -> dict[str, str]:
        env = {
            "EVENT_NAME": "pull_request",
            "GITHUB_REPOSITORY": REPO,
            "HEAD_REPOSITORY": REPO,
            "HEAD_REF": "fix/example",
            "PR_NUMBER": "3401",
            "PR_LABELS": "[]",
        }
        env.update(extra)
        return env

    def test_run_escalates_planted_pr_3401_and_keeps_docs_only_light(self) -> None:
        def fetch_3401(path: str) -> object:
            return [{"filename": p} for p in PR_3401_PATHS] if "/files" in path else []

        def fetch_docs(path: str) -> object:
            return [{"filename": "docs/usage/cli.md"}] if "/files" in path else []

        with redirect_stderr(io.StringIO()):
            full = ci_tier.run(self.base_env(), ci_tier.DEFAULT_CONFIG, fetch_3401)
            light = ci_tier.run(self.base_env(), ci_tier.DEFAULT_CONFIG, fetch_docs)
        self.assertEqual(full.tier, ci_tier.FULL)
        self.assertIn("scripts/lib/safe_subprocess.py", full.reason)
        self.assertEqual(light.tier, ci_tier.LIGHT)

    def test_run_fails_closed_when_paths_cannot_be_obtained(self) -> None:
        def broken_files(path: str) -> object:
            if "/files" in path:
                raise ci_tier.TierError("files endpoint down")
            return []

        with redirect_stderr(io.StringIO()):
            no_token = ci_tier.run(self.base_env(), ci_tier.DEFAULT_CONFIG, None)
            no_number = ci_tier.run(
                self.base_env(PR_NUMBER=""),
                ci_tier.DEFAULT_CONFIG,
                lambda path: [],
            )
            api_error = ci_tier.run(self.base_env(), ci_tier.DEFAULT_CONFIG, broken_files)
        for label, decision in (
            ("no_token", no_token),
            ("no_number", no_number),
            ("api_error", api_error),
        ):
            with self.subTest(case=label):
                self.assertEqual(decision.tier, ci_tier.FULL)
                self.assertEqual(decision.reason, "changed paths unavailable")

    def test_run_fetches_pull_request_files_at_most_once_across_release_and_full_paths(
        self,
    ) -> None:
        file_fetches = 0

        def fetch(path: str) -> object:
            nonlocal file_fetches
            if "/files" in path:
                file_fetches += 1
                return [{"filename": "scripts/lib/safe_subprocess.py"}]
            return []

        env = self.base_env(HEAD_REF=RELEASE_REF, PR_AUTHOR="lusoris", PR_AUTHOR_TYPE="User")
        with redirect_stderr(io.StringIO()):
            decision = ci_tier.run(env, ci_tier.DEFAULT_CONFIG, fetch)
        self.assertEqual(file_fetches, 1)
        self.assertEqual(decision.tier, ci_tier.FULL)
        self.assertIn("scripts/lib/safe_subprocess.py", decision.reason)


class FullPathsDrift(unittest.TestCase):
    """Drift check ``--check-full-paths`` against tracked files in git."""

    @staticmethod
    def _git_env() -> dict[str, str]:
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        env["GIT_CONFIG_GLOBAL"] = os.devnull
        env["GIT_CONFIG_NOSYSTEM"] = "1"
        return env

    def _run_git(self, root: Path, *args: str) -> None:
        git = ci_tier.GIT
        if git is None:
            self.fail("git executable not found")
        run_command(
            [git, "-C", str(root), *args],
            allowed_executables=(git,),
            env=self._git_env(),
            text=True,
            capture_output=True,
            check=True,
            timeout_seconds=ci_tier.API_TIMEOUT_S,
        )

    def test_current_tree_passes_full_paths_drift_check(self) -> None:
        err = io.StringIO()
        with redirect_stderr(err):
            rc = ci_tier.main(["--check-full-paths"])
        self.assertEqual(rc, 0, err.getvalue())

    def test_renamed_listed_file_fails_drift_check_and_restored_file_passes(self) -> None:
        root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, root, True)
        self._run_git(root, "init", "-q")
        cfg_dir = root / ".github"
        cfg_dir.mkdir(parents=True, exist_ok=True)
        (cfg_dir / "ci-tier.json").write_text(
            ci_tier.DEFAULT_CONFIG.read_text(encoding="utf-8"),
            encoding="utf-8",
        )
        for rel in CONFIG["full_paths"]:
            target = root / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("# stub\n", encoding="utf-8")
        self._run_git(root, "add", ".")

        err_ok = io.StringIO()
        with redirect_stderr(err_ok):
            self.assertEqual(
                ci_tier.main(["--check-full-paths", "--repo-root", str(root)]),
                0,
                err_ok.getvalue(),
            )

        self._run_git(
            root,
            "mv",
            "scripts/lib/safe_subprocess.py",
            "scripts/lib/safe_subprocess_renamed.py",
        )
        err_fail = io.StringIO()
        with redirect_stderr(err_fail):
            self.assertEqual(ci_tier.main(["--check-full-paths", "--repo-root", str(root)]), 1)
        self.assertIn("scripts/lib/safe_subprocess.py", err_fail.getvalue())

        self._run_git(
            root,
            "mv",
            "scripts/lib/safe_subprocess_renamed.py",
            "scripts/lib/safe_subprocess.py",
        )
        err_restored = io.StringIO()
        with redirect_stderr(err_restored):
            self.assertEqual(
                ci_tier.main(["--check-full-paths", "--repo-root", str(root)]),
                0,
                err_restored.getvalue(),
            )


class Main(unittest.TestCase):
    def test_a_broken_definition_fails_the_job_loudly(self) -> None:
        directory = tempfile.mkdtemp()
        output = Path(directory) / "out"
        broken = Path(directory) / "broken.json"
        broken.write_text("{}", encoding="utf-8")
        env = {"EVENT_NAME": "pull_request"}
        with mock.patch.dict(os.environ, env, clear=True), redirect_stdout(io.StringIO()):
            with redirect_stderr(io.StringIO()), self.assertRaises(ci_tier.TierError):
                ci_tier.main(["--config", str(broken), "--github-output", str(output)])

    def test_main_writes_the_outputs_and_the_lists_the_aggregator_reads(self) -> None:
        directory = tempfile.mkdtemp()
        output = Path(directory) / "out"
        env = {
            "EVENT_NAME": "pull_request",
            "GH_TOKEN": "t",
            "PR_NUMBER": "7",
            "GITHUB_REPOSITORY": REPO,
            "HEAD_REPOSITORY": REPO,
            "HEAD_REF": "fix/example",
            "PR_LABELS": "[]",
        }

        def fake_api(method: str, url: str, token: str, payload: object | None = None) -> object:
            return [{"filename": "docs/usage/cli.md"}] if "/files" in url else []

        with (
            mock.patch.dict(os.environ, env, clear=True),
            mock.patch.object(ci_tier, "api_request", side_effect=fake_api),
            redirect_stdout(io.StringIO()),
            redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(ci_tier.main(["--github-output", str(output)]), 0)
        written = dict(line.split("=", 1) for line in output.read_text().splitlines())
        self.assertEqual(written["tier"], "light")
        self.assertEqual(json.loads(written["always_json"]), CONFIG["always"])
        self.assertEqual(json.loads(written["full_only_json"]), CONFIG["full_only"])

    def test_main_without_token_fails_closed_to_full(self) -> None:
        directory = tempfile.mkdtemp()
        output = Path(directory) / "out"
        env = {
            "EVENT_NAME": "pull_request",
            "GITHUB_REPOSITORY": REPO,
            "HEAD_REPOSITORY": REPO,
            "HEAD_REF": "fix/example",
            "PR_LABELS": "[]",
        }
        with mock.patch.dict(os.environ, env, clear=True), redirect_stdout(io.StringIO()):
            self.assertEqual(ci_tier.main(["--github-output", str(output)]), 0)
        written = dict(line.split("=", 1) for line in output.read_text().splitlines())
        self.assertEqual(written["tier"], "full")
        self.assertEqual(written["reason"], "changed paths unavailable")

    def test_a_failing_decision_falls_back_to_the_full_tier(self) -> None:
        directory = tempfile.mkdtemp()
        output = Path(directory) / "out"
        env = {
            "EVENT_NAME": "pull_request",
            "GH_TOKEN": "t",
            "PR_NUMBER": "7",
            "GITHUB_REPOSITORY": REPO,
        }
        boom = ci_tier.TierError("api down")
        with (
            mock.patch.dict(os.environ, env, clear=True),
            mock.patch.object(ci_tier, "api_request", side_effect=boom),
            redirect_stdout(io.StringIO()),
            redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(ci_tier.main(["--github-output", str(output)]), 0)
        self.assertIn("tier=full", output.read_text())


if __name__ == "__main__":
    unittest.main()
