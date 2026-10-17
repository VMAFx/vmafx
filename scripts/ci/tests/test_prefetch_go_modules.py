#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/ci/prefetch_go_modules.py retries a failed download and fails loudly.

The tests put a stand-in `go` alone on PATH and the tool cache in a scratch
directory. The stand-in records every call and fails the first calls of a kind
(`mod`, `install`) as its plan says, with the text the module proxy gave on
2026-10-10 (T-CI-GO-MODULES-DOWNLOADED-IN-TESTS-2026-10-10); a successful
`install` writes a buf that reports the version it was asked for.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "ci" / "prefetch_go_modules.py"
PROXY_ERROR = (
    'go.opentelemetry.io/otel@v1.47.0: read "https://proxy.golang.org/go.opentelemetry.io/'
    'otel/@v/v1.47.0.zip": stream error: stream ID 33; INTERNAL_ERROR; received from peer'
)
STAND_IN_GO = """#!{python}
import json, os, stat, sys, time
state = os.environ["STAND_IN_GO_STATE"]
kind = sys.argv[1]
with open(os.path.join(state, "calls.jsonl"), "a", encoding="utf-8") as log:
    log.write(json.dumps([*sys.argv[1:], os.environ.get("GOBIN", "")]) + "\\n")
with open(os.path.join(state, "calls.jsonl"), encoding="utf-8") as log:
    seen = sum(1 for line in log if json.loads(line)[0] == kind)
with open(os.path.join(state, "plan.json"), encoding="utf-8") as plan_file:
    plan = json.load(plan_file)
time.sleep(plan.get("sleep", 0))
failures = plan.get(kind, 0)
if failures < 0 or seen <= failures:
    sys.stderr.write(plan["error"] + "\\n")
    sys.exit(1)
if kind == "install" and not plan.get("installs_nothing"):
    version = plan.get("reports") or sys.argv[2].rsplit("@v", 1)[1]
    os.makedirs(os.environ["GOBIN"], exist_ok=True)
    buf = os.path.join(os.environ["GOBIN"], "buf")
    with open(buf, "w", encoding="utf-8") as out:
        out.write("#!{python}\\nprint(" + repr(version) + ")\\n")
    os.chmod(buf, os.stat(buf).st_mode | stat.S_IXUSR)
"""


def load() -> ModuleType:
    spec = importlib.util.spec_from_file_location("prefetch_go_modules", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


PREFETCH = load()


def buf_pin() -> str:
    pin = re.search(r'^BUF_VERSION="([^"]+)"', (ROOT / "build-config.env").read_text("utf-8"), re.M)
    assert pin is not None
    return pin.group(1)


class StandInGo:
    """A directory holding a `go` that follows a plan, alone on PATH, and an
    empty tool cache."""

    def __init__(self, tmp: str, **plan: object) -> None:
        self.state = Path(tmp) / "state"
        self.state.mkdir()
        self.bin = Path(tmp) / "bin"
        self.bin.mkdir()
        self.cache = Path(tmp) / "cache"
        go = self.bin / "go"
        go.write_text(STAND_IN_GO.format(python=sys.executable), encoding="utf-8")
        go.chmod(0o755)
        (self.state / "plan.json").write_text(
            json.dumps({"error": PROXY_ERROR, **plan}), encoding="utf-8"
        )

    def environment(self) -> dict[str, str]:
        return {
            "PATH": str(self.bin),
            "STAND_IN_GO_STATE": str(self.state),
            "XDG_CACHE_HOME": str(self.cache),
        }

    def tool_cache(self) -> Path:
        return self.cache / "vmafx-tools" / f"buf-{buf_pin()}"

    def calls(self, kind: str) -> list[list[str]]:
        """The calls of one kind, each as its arguments and then its GOBIN."""
        log = self.state / "calls.jsonl"
        if not log.exists():
            return []
        calls = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
        return [call for call in calls if call[0] == kind]


def run_prefetch(
    environment: dict[str, str], delays: tuple[float, ...]
) -> tuple[int, list[float], str, str]:
    """(exit status, the waits it asked for, stdout, stderr) of one prefetch."""
    waits: list[float] = []
    out, err = io.StringIO(), io.StringIO()
    with (
        mock.patch.dict(os.environ, environment),
        contextlib.redirect_stdout(out),
        contextlib.redirect_stderr(err),
    ):
        os.environ.pop("GITHUB_ACTIONS", None)
        status = PREFETCH.prefetch(ROOT, delays, waits.append)
    return status, waits, out.getvalue(), err.getvalue()


@unittest.skipIf(os.name == "nt", "the stand-in go is a POSIX script")
class PrefetchGoModules(unittest.TestCase):
    def test_a_dropped_stream_is_retried_until_the_download_succeeds(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            go = StandInGo(tmp, mod=2)
            status, waits, out, _ = run_prefetch(go.environment(), PREFETCH.RETRY_DELAYS_S)
            modules, installs = go.calls("mod"), go.calls("install")
        self.assertEqual(status, 0, out)
        self.assertEqual(waits, list(PREFETCH.RETRY_DELAYS_S[:2]))
        self.assertEqual(modules, [["mod", "download", ""]] * 3)
        self.assertEqual(len(installs), 1)
        self.assertIn("attempt 1 of 5 failed, next in 10 s", out)
        self.assertIn("INTERNAL_ERROR", out)
        self.assertIn("done (attempt 3)", out)

    def test_a_download_that_never_succeeds_fails_the_step_with_the_last_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            go = StandInGo(tmp, mod=-1)
            status, waits, _, err = run_prefetch(go.environment(), PREFETCH.RETRY_DELAYS_S)
            modules, installs = go.calls("mod"), go.calls("install")
        self.assertEqual(status, 1)
        self.assertEqual(waits, list(PREFETCH.RETRY_DELAYS_S))
        # Every attempt ran, and nothing after the download that failed.
        self.assertEqual(modules, [["mod", "download", ""]] * PREFETCH.ATTEMPTS)
        self.assertEqual(installs, [])
        self.assertIn("could not download the modules go.mod requires", err)
        self.assertIn("in 5 attempts over 220 s of waiting", err)
        self.assertIn("test_crd_compat", err)
        self.assertIn(PROXY_ERROR, err)

    def test_buf_is_installed_into_the_tool_cache_and_its_download_retried(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            go = StandInGo(tmp, install=1)
            status, waits, out, _ = run_prefetch(go.environment(), (0.0, 0.0))
            installs = go.calls("install")
            with mock.patch.dict(os.environ, go.environment()):
                command = PREFETCH.proto_generate().buf_command(ROOT)
            cache = str(go.tool_cache())
        self.assertEqual(status, 0, out)
        self.assertEqual(waits, [0.0])
        package = f"github.com/bufbuild/buf/cmd/buf@v{buf_pin()}"
        self.assertEqual(installs, [["install", package, cache]] * 2)
        # The generator now runs that copy, not `go run`.
        self.assertEqual(command, [str(Path(cache) / "buf")])

    def test_an_installed_buf_is_not_downloaded_again(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            go = StandInGo(tmp)
            first = run_prefetch(go.environment(), (0.0,))
            second = run_prefetch(go.environment(), (0.0,))
            installs = go.calls("install")
        self.assertEqual((first[0], second[0]), (0, 0))
        self.assertEqual(len(installs), 1)
        self.assertIn("in the tool cache already", second[2])

    def test_an_install_that_leaves_no_pinned_buf_fails_the_step(self) -> None:
        for plan in ({"installs_nothing": True}, {"reports": "0.0.1"}):
            with self.subTest(plan=plan), tempfile.TemporaryDirectory() as tmp:
                go = StandInGo(tmp, **plan)
                status, _, _, err = run_prefetch(go.environment(), (0.0,))
                with mock.patch.dict(os.environ, go.environment()):
                    command = PREFETCH.proto_generate().buf_command(ROOT)
                self.assertEqual(status, 1)
                self.assertIn("holds no buf that reports the pinned version", err)
                # A copy that is not the pinned release is never run.
                self.assertEqual(command[:2], ["go", "run"])

    def test_an_attempt_that_hangs_is_cut_off_and_retried(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            go = StandInGo(tmp, sleep=30)
            with mock.patch.object(PREFETCH, "ATTEMPT_TIMEOUT_S", 0.5):
                status, waits, _, err = run_prefetch(go.environment(), (0.0,))
            modules = go.calls("mod")
        self.assertEqual(status, 1)
        self.assertEqual(waits, [0.0])
        self.assertEqual(modules, [["mod", "download", ""]] * 2)
        self.assertIn("ran over 0.5 s", err)

    def test_without_go_nothing_is_downloaded_and_the_reason_is_printed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            status, waits, out, _ = run_prefetch({"PATH": tmp}, PREFETCH.RETRY_DELAYS_S)
        self.assertEqual((status, waits), (0, []))
        self.assertIn("SKIP: go is not on PATH", out)
        for test in PREFETCH.GO_TOOL_TESTS:
            self.assertIn(test, out)

    def test_a_failure_is_an_error_annotation_on_github_actions(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            go = StandInGo(tmp, mod=-1)
            out, err = io.StringIO(), io.StringIO()
            environment = {**go.environment(), "GITHUB_ACTIONS": "true"}
            with (
                mock.patch.dict(os.environ, environment),
                contextlib.redirect_stdout(out),
                contextlib.redirect_stderr(err),
            ):
                status = PREFETCH.prefetch(ROOT, (), lambda _: None)
        self.assertEqual(status, 1)
        annotations = [line for line in out.getvalue().splitlines() if line.startswith("::")]
        self.assertEqual(len(annotations), 1)
        self.assertTrue(annotations[0].startswith("::error::prefetch_go_modules: could not"))

    def test_the_waits_spread_the_attempts_over_minutes(self) -> None:
        self.assertEqual(PREFETCH.ATTEMPTS, len(PREFETCH.RETRY_DELAYS_S) + 1)
        self.assertGreaterEqual(PREFETCH.ATTEMPTS, 4)
        self.assertGreaterEqual(sum(PREFETCH.RETRY_DELAYS_S), 180)
        self.assertEqual(sorted(PREFETCH.RETRY_DELAYS_S), list(PREFETCH.RETRY_DELAYS_S))


if __name__ == "__main__":
    unittest.main()
