#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative, boundary, and workflow contract tests for upstream_watch_count.sh (ADR-0448)."""

from __future__ import annotations

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_PATH = REPO_ROOT / "scripts" / "ci" / "upstream_watch_count.sh"
WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

WORKFLOW_FILES = (
    WORKFLOWS_DIR / "upstream-ffmpeg-hip-hwdec-watcher.yml",
    WORKFLOWS_DIR / "upstream-netflix-645-hdr-model-watcher.yml",
    WORKFLOWS_DIR / "upstream-netflix-955-watcher.yml",
)


class UpstreamWatchCountTests(unittest.TestCase):
    """Test suite verifying upstream_watch_count.sh behavior and workflow contracts."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.bin_dir = Path(self.tmp.name) / "bin"
        self.bin_dir.mkdir(parents=True, exist_ok=True)
        self.gh_bin = self.bin_dir / "gh"

    def _create_fake_gh(self, script_body: str) -> None:
        self.gh_bin.write_text(f"#!/usr/bin/env bash\n{script_body}\n", encoding="utf-8")
        self.gh_bin.chmod(self.gh_bin.stat().st_mode | stat.S_IXUSR | stat.S_IRUSR)

    def _run_script(
        self,
        *args: str,
        sleep_scale: str = "0",
        extra_env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ)
        env["PATH"] = f"{self.bin_dir}:{env.get('PATH', '')}"
        env["UPSTREAM_WATCH_SLEEP_SCALE"] = sleep_scale
        if extra_env:
            env.update(extra_env)
        return subprocess.run(  # noqa: S603 -- fixed bash executable and script argv
            ["/bin/bash", str(SCRIPT_PATH), *args],
            capture_output=True,
            text=True,
            check=False,
            env=env,
        )

    def test_positive_returns_integer_and_exits_zero(self) -> None:
        """Positive test: prints 3 -> exit 0, stdout 3."""
        self._create_fake_gh("echo 3; exit 0")
        proc = self._run_script("api", "search/code", "-f", "q=test", "--jq", ".total_count")
        self.assertEqual(proc.returncode, 0, f"Expected 0, got {proc.returncode}: {proc.stderr}")
        self.assertEqual(proc.stdout.strip(), "3")
        self.assertNotIn("::error::", proc.stderr)

    def test_negative_prints_rate_limit_json_exits_one(self) -> None:
        """Negative test: prints rate-limit JSON every time -> exit 1, no integer on stdout."""
        self._create_fake_gh('echo \'{"message":"try again in 8.8s"}\'; exit 1')
        proc = self._run_script(
            "api",
            "search/code",
            "-f",
            "q=test",
            "--jq",
            ".total_count",
            sleep_scale="0",
        )
        self.assertEqual(proc.returncode, 1, f"Expected 1, got {proc.returncode}")
        self.assertEqual(proc.stdout.strip(), "", "Stdout must not contain any output on failure")
        self.assertIn("::error::", proc.stderr)
        self.assertIn("try again in 8.8s", proc.stderr)

    def test_boundary_fails_twice_then_prints_zero_exits_zero(self) -> None:
        """Boundary test: fails twice then prints 0 -> exit 0 stdout 0."""
        state_file = Path(self.tmp.name) / "calls.txt"
        fake_gh = f"""
count=0
if [ -f "{state_file}" ]; then
  count=$(cat "{state_file}")
fi
count=$((count + 1))
echo "$count" > "{state_file}"

if [ "$count" -lt 3 ]; then
  echo '{{"message":"rate limit exceeded"}}'
  exit 1
fi
echo 0
exit 0
"""
        self._create_fake_gh(fake_gh)
        proc = self._run_script(
            "-X",
            "GET",
            "search/code",
            "-f",
            "q=test",
            "--jq",
            ".total_count",
            sleep_scale="0",
        )
        self.assertEqual(proc.returncode, 0, f"Expected 0, got {proc.returncode}: {proc.stderr}")
        self.assertEqual(proc.stdout.strip(), "0")
        self.assertNotIn("::error::", proc.stderr)
        self.assertEqual(state_file.read_text(encoding="utf-8").strip(), "3")

    def test_non_integer_output_fails_closed(self) -> None:
        """Non-integer output (e.g. JSON printed on exit 0) fails closed with exit 1."""
        self._create_fake_gh('echo \'{"message":"not an integer"}\'; exit 0')
        proc = self._run_script("api", "search/code", sleep_scale="0")
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(proc.stdout.strip(), "")
        self.assertIn("::error::", proc.stderr)

    def _counting_gh(self, replies: int) -> Path:
        """Fake gh: print a stated-delay 429 for the first `replies` calls, then 3."""
        state_file = Path(self.tmp.name) / "calls.txt"
        self._create_fake_gh(f"""
count=0
if [ -f "{state_file}" ]; then
  count=$(cat "{state_file}")
fi
count=$((count + 1))
echo "$count" > "{state_file}"
if [ "$count" -le {replies} ]; then
  echo '{{"message":"try again in 84.556726929s","status":"429"}}'
  echo 'gh: try again in 84.556726929s (HTTP 429)' >&2
  exit 1
fi
echo 3
exit 0
""")
        return state_file

    def _recording_sleep(self) -> Path:
        """Fake sleep on PATH: records each requested delay instead of waiting."""
        log = Path(self.tmp.name) / "sleeps.txt"
        sleep_bin = self.bin_dir / "sleep"
        sleep_bin.write_text(f'#!/usr/bin/env bash\necho "$1" >> "{log}"\n', encoding="utf-8")
        sleep_bin.chmod(sleep_bin.stat().st_mode | stat.S_IXUSR | stat.S_IRUSR)
        return log

    def _sleeps(self, log: Path) -> list[str]:
        return log.read_text(encoding="utf-8").split() if log.is_file() else []

    def test_stated_delay_is_waited_then_success(self) -> None:
        """Positive: a 429 stating 84.56 s waits 86 s (not the 15 s step), then the count is returned."""
        calls = self._counting_gh(1)
        log = self._recording_sleep()
        proc = self._run_script("api", "search/code", sleep_scale="1")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.strip(), "3")
        self.assertEqual(self._sleeps(log), ["86"])
        self.assertEqual(calls.read_text(encoding="utf-8").strip(), "2")

    def test_three_stated_429s_stay_red(self) -> None:
        """Negative: three 429s exhaust the three attempts and the run fails with the reply."""
        calls = self._counting_gh(3)
        log = self._recording_sleep()
        proc = self._run_script("api", "search/code", sleep_scale="1")
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(proc.stdout.strip(), "")
        self.assertIn("try again in 84.556726929s", proc.stderr)
        self.assertEqual(self._sleeps(log), ["86", "86"])
        self.assertEqual(calls.read_text(encoding="utf-8").strip(), "3")

    def test_stated_delay_above_bound_fails_at_once(self) -> None:
        """Boundary: a stated delay above the per-retry bound fails without waiting."""
        calls = self._counting_gh(3)
        log = self._recording_sleep()
        proc = self._run_script(
            "api", "search/code", sleep_scale="1", extra_env={"UPSTREAM_WATCH_MAX_WAIT_S": "85"}
        )
        self.assertEqual(proc.returncode, 1)
        self.assertIn("exceeds the 85s bound", proc.stderr)
        self.assertEqual(self._sleeps(log), [])
        self.assertEqual(calls.read_text(encoding="utf-8").strip(), "1")

    def test_total_wait_is_bounded(self) -> None:
        """Boundary: the second wait would pass the total bound, so the run fails after two calls."""
        calls = self._counting_gh(3)
        log = self._recording_sleep()
        proc = self._run_script(
            "api", "search/code", sleep_scale="1", extra_env={"UPSTREAM_WATCH_TOTAL_WAIT_S": "171"}
        )
        self.assertEqual(proc.returncode, 1)
        self.assertIn("171s total bound", proc.stderr)
        self.assertEqual(self._sleeps(log), ["86"])
        self.assertEqual(calls.read_text(encoding="utf-8").strip(), "2")

    def test_workflows_contract(self) -> None:
        """No workflow swallows a gh error, every one creates its label, the count queries use the helper."""
        for workflow in WORKFLOW_FILES:
            self.assertTrue(workflow.is_file(), f"Workflow file missing: {workflow}")
            content = workflow.read_text(encoding="utf-8")
            self.assertNotIn(
                "|| echo 0", content, f"Swallowed error '|| echo 0' found in {workflow.name}"
            )
            self.assertIn(
                "gh label create upstream-merged",
                content,
                f"Missing upstream-merged label creation in {workflow.name}",
            )

        # Count-bearing workflows specifically invoke upstream_watch_count.sh for their count queries
        ffmpeg_content = WORKFLOW_FILES[0].read_text(encoding="utf-8")
        self.assertIn("FILES_HIT=$(scripts/ci/upstream_watch_count.sh", ffmpeg_content)
        self.assertIn("ENUM_HIT=$(scripts/ci/upstream_watch_count.sh", ffmpeg_content)

        netflix_645_content = WORKFLOW_FILES[1].read_text(encoding="utf-8")
        self.assertIn("MODEL_HIT=$(scripts/ci/upstream_watch_count.sh", netflix_645_content)
        self.assertNotIn('|| echo ""', netflix_645_content)

    def test_no_workflow_uses_code_search(self) -> None:
        """Code search is throttled for the workflow token; the watchers read the contents API."""
        for workflow in WORKFLOW_FILES:
            content = workflow.read_text(encoding="utf-8")
            self.assertNotIn("search/code", content, f"code-search call in {workflow.name}")
            self.assertNotIn("gh search code", content, f"code-search hint in {workflow.name}")
        ffmpeg_content = WORKFLOW_FILES[0].read_text(encoding="utf-8")
        self.assertIn("repos/FFmpeg/FFmpeg/contents/libavutil", ffmpeg_content)
        self.assertIn("repos/FFmpeg/FFmpeg/contents/libavutil/hwcontext.h", ffmpeg_content)


if __name__ == "__main__":
    unittest.main()
