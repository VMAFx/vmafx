#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/test/fetch-test-yuvs.sh outlasts a failing host, with one retry policy.

On 2026-10-10 the fixture host answered HTTP 504 for some minutes. The script
tried six times within about ten seconds (`curl --retry 5 --retry-delay 2`) and
the Python harness's download_reactively() tried once; two required jobs failed
(T-CI-FIXTURE-DOWNLOADS-SHORT-RETRY-2026-10-10).

The tests source the script and call its main() with a fixture list of their
own, against a stand-in server that answers 504 before 200. The retry runs for
real here (one wait of the policy's first delay); the policy's length and its
other entry points are tested in compat/python-vmaf/tests/test_retrying_download.py.
"""

from __future__ import annotations

import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.http_stand_in import StandIn  # noqa: E402

SCRIPT = ROOT / "scripts" / "test" / "fetch-test-yuvs.sh"
POLICY = "compat/python-vmaf/tools/retrying_download.py"
BASH = shutil.which("bash")
FIRST = b"first fixture\n" * 32
SECOND = b"second fixture\n" * 32
BODIES = {"/yuv/first.yuv": FIRST, "/yuv/second.yuv": SECOND}
# The script's summary line: already-ok, replaced, fetched, failed.
SUMMARY = "fetch-test-yuvs.sh: {} already-ok, {} replaced, {} fetched, {} failed"


def md5(data: bytes) -> str:
    return hashlib.md5(data, usedforsecurity=False).hexdigest()


def fetch(
    server: StandIn, directory: Path, *names: str, second_md5: str = md5(SECOND)
) -> subprocess.CompletedProcess[str]:
    """Source the script, point it at the stand-in server and run main()."""
    assert BASH is not None
    program = (
        'source "$1"; shift\n'
        f'FIXTURES=("first.yuv {md5(FIRST)}" "second.yuv {second_md5}")\n'
        'YUV_DIR="$1"; BASE_URL="$2"; shift 2\n'
        'main "$@"\n'
    )
    return subprocess.run(  # noqa: S603 (bash and this repository's script)
        [BASH, "-c", program, "fetch", str(SCRIPT), str(directory), server.url("yuv"), *names],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )


@unittest.skipIf(BASH is None or shutil.which("md5sum") is None, "needs bash and md5sum")
class FetchTestYuvs(unittest.TestCase):
    def test_a_504_before_200_is_outlasted(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, StandIn([504], bodies=BODIES) as server:
            done = fetch(server, Path(tmp))
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(server.requests, ["/yuv/first.yuv"] * 2 + ["/yuv/second.yuv"])
            self.assertEqual((Path(tmp) / "first.yuv").read_bytes(), FIRST)
            self.assertEqual(
                sorted(p.name for p in Path(tmp).iterdir()), ["first.yuv", "second.yuv"]
            )
        self.assertIn(
            "attempt 1 of 7 failed (HTTP Error 504: Gateway Timeout), next in 5 s", done.stderr
        )
        self.assertIn(SUMMARY.format(0, 0, 2, 0), done.stdout)

    def test_a_missing_file_fails_at_once_and_ends_the_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, StandIn([404], bodies=BODIES) as server:
            done = fetch(server, Path(tmp))
            self.assertEqual(done.returncode, 1)
            # One request: no retry of a 404, and no attempt at the second file.
            self.assertEqual(server.requests, ["/yuv/first.yuv"])
            self.assertEqual(list(Path(tmp).iterdir()), [])
        self.assertIn("HTTP Error 404", done.stderr)
        self.assertIn(
            "download failed for first.yuv; not trying the fixtures after it", done.stderr
        )
        self.assertIn(SUMMARY.format(0, 0, 0, 1), done.stdout)

    def test_a_download_with_the_wrong_content_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, StandIn(bodies=BODIES) as server:
            done = fetch(server, Path(tmp), second_md5="0" * 32)
            self.assertEqual(done.returncode, 1)
            self.assertEqual(len(server.requests), 2)
        self.assertIn("second.yuv md5 mismatch after download", done.stderr)
        self.assertIn(SUMMARY.format(0, 0, 1, 1), done.stdout)

    def test_only_the_named_fixtures_are_fetched(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, StandIn(bodies=BODIES) as server:
            done = fetch(server, Path(tmp), "second.yuv")
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(server.requests, ["/yuv/second.yuv"])
            again = fetch(server, Path(tmp), "second.yuv")
            self.assertEqual(server.requests, ["/yuv/second.yuv"])
        self.assertIn("ok      second.yuv (md5 verified)", again.stdout)
        self.assertIn(SUMMARY.format(1, 0, 0, 0), again.stdout)

    def test_a_name_that_is_no_fixture_is_refused_before_any_request(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, StandIn(bodies=BODIES) as server:
            done = fetch(server, Path(tmp), "first.yuv", "third.yuv")
            self.assertEqual(server.requests, [])
        self.assertEqual(done.returncode, 2)
        self.assertIn("third.yuv is not a fixture of this script", done.stderr)


class OneRetryPolicy(unittest.TestCase):
    """Both fixture downloads go through vmaf/tools/retrying_download.py."""

    def test_the_script_downloads_through_the_policy_and_nothing_else(self) -> None:
        text = SCRIPT.read_text(encoding="utf-8")
        code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
        self.assertIn(f'DOWNLOAD="${{REPO_ROOT}}/{POLICY}"', code)
        self.assertIn('python3 -I "${DOWNLOAD}" "${BASE_URL}/${name}" "${target}"', code)
        self.assertNotRegex(code, r"\b(curl|wget)\b")
        self.assertTrue((ROOT / POLICY).is_file())

    def test_the_harness_downloads_through_the_policy_and_nothing_else(self) -> None:
        text = (ROOT / "compat" / "python-vmaf" / "config.py").read_text(encoding="utf-8")
        code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
        self.assertIn("from vmaf.tools.retrying_download import retrieve", code)
        self.assertEqual(len(re.findall(r"\bretrieve\(remote_path, tmp_path\)", code)), 1)
        self.assertNotRegex(code, r"\b(urlretrieve|urlopen)\(")

    def test_no_workflow_fetches_the_fixtures_on_its_own(self) -> None:
        # A job that needs fixtures runs the script (or the harness); an inline
        # download of the fixture repository has no retry policy and no md5.
        for path in sorted((ROOT / ".github" / "workflows").glob("*.yml")):
            text = path.read_text(encoding="utf-8")
            self.assertNotRegex(text, r"vmaf_resource/raw/", path.name)

    def test_the_waits_are_longer_than_the_six_attempts_in_ten_seconds(self) -> None:
        text = (ROOT / POLICY).read_text(encoding="utf-8")
        found = re.search(r"(?m)^RETRY_DELAYS_S: tuple\[float, \.\.\.\] = \(([^)]*)\)$", text)
        assert found is not None
        delays = [float(value) for value in found.group(1).split(",") if value.strip()]
        self.assertGreaterEqual(sum(delays), 180)


if __name__ == "__main__":
    unittest.main()
