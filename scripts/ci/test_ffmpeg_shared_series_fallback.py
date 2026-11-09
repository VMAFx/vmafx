"""Exercise the shared series release download fallback through the forge API."""

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

from __future__ import annotations

import shutil
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci import ffmpeg_shared_series_fixture as shared_series
from scripts.ci.ffmpeg_patch_stack import (
    SHARED_SERIES_MAX_DOWNLOADS,
    shared_series_timeout_seconds,
)
from scripts.lib.safe_subprocess import TextCommandResult
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/ci/ffmpeg-shared-series.sh"


class SharedSeriesFallbackTest(unittest.TestCase):
    """Verify forge API fallback behavior when direct download fails."""

    def setUp(self) -> None:
        """Create an isolated test directory and staging fixtures."""
        self.temp_dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.temp_dir, True)
        self.fixture_dir = self.temp_dir / "fixture"
        self.tarball_path, self.digest = shared_series.tarball(
            self.fixture_dir,
            base_tag="n9.0.2",
            base_commit="commit-fixture",
            patches={"0001-shared-fix.patch": "fake-patch-content\n"},
        )
        self.bin_dir = self.temp_dir / "bin"
        self.bin_dir.mkdir()
        tools = [
            "bash",
            "dirname",
            "mktemp",
            "rm",
            "mkdir",
            "mv",
            "cat",
            "awk",
            "sed",
            "tr",
            "head",
            "cut",
            "wc",
            "tar",
            "gzip",
            "sha256sum",
            "git",
            "sleep",
            "kill",
            "python3",
            "timeout",
            "gtimeout",
            "perl",
        ]
        for tool in tools:
            path = shutil.which(tool)
            if path:
                (self.bin_dir / tool).symlink_to(path)
        self.write_stub_curl()
        self.write_stub_gh()

    def write_stub_curl(self) -> None:
        """Write the stub curl executable into the isolated bin directory."""
        curl_path = self.bin_dir / "curl"
        curl_path.write_text(
            "#!/usr/bin/env python3\n"
            '"""Stub curl executable."""\n'
            "import os, sys, shutil\n"
            "from pathlib import Path\n"
            'mode = os.environ.get("CURL_STUB_MODE", "fail")\n'
            'if mode == "fail":\n'
            '    sys.stderr.write("curl: (22) 504 Gateway Timeout\\n")\n'
            "    sys.exit(22)\n"
            'elif mode == "ok":\n'
            "    dest = None\n"
            "    url = None\n"
            "    args = sys.argv[1:]\n"
            "    i = 0\n"
            "    while i < len(args):\n"
            '        if args[i] == "--output" and i + 1 < len(args):\n'
            "            dest = args[i + 1]\n"
            "            i += 2\n"
            '        elif not args[i].startswith("-"):\n'
            "            url = args[i]\n"
            "            i += 1\n"
            "        else:\n"
            "            i += 1\n"
            '    fixture_dir = Path(os.environ["GH_STUB_FIXTURE_DIR"])\n'
            '    asset = url.rsplit("/", 1)[-1] if url else ""\n'
            "    src = fixture_dir / asset\n"
            "    if dest and src.exists():\n"
            "        shutil.copyfile(src, dest)\n"
            "        sys.exit(0)\n"
            "    sys.exit(1)\n"
            "sys.exit(22)\n"
        )
        curl_path.chmod(0o755)

    def write_stub_gh(self) -> None:
        """Write the stub gh executable into the isolated bin directory."""
        gh_path = self.bin_dir / "gh"
        gh_path.write_text(
            "#!/usr/bin/env python3\n"
            '"""Stub gh executable."""\n'
            "import os, sys\n"
            "from pathlib import Path\n"
            'argv_file = os.environ.get("GH_STUB_ARGV_FILE")\n'
            "if argv_file:\n"
            '    with open(argv_file, "a", encoding="utf-8") as f:\n'
            '        f.write(" ".join(sys.argv[1:]) + "\\n")\n'
            'mode = os.environ.get("GH_STUB_MODE", "ok")\n'
            'if mode == "fail":\n'
            '    sys.stderr.write("gh: release download failed\\n")\n'
            "    sys.exit(1)\n"
            'elif mode == "hang":\n'
            '    sleep_arg = os.environ.get("GH_STUB_SLEEP_ARG", "30.71829")\n'
            '    os.execvp("sleep", ["sleep", sleep_arg])\n'
            'elif mode == "oversize":\n'
            "    sys.stdout.buffer.write(b'0' * 67108865)\n"
            "    sys.exit(0)\n"
            'elif mode == "wrong_bytes":\n'
            "    args = sys.argv[1:]\n"
            '    pattern = ""\n'
            "    for i in range(len(args) - 1):\n"
            '        if args[i] == "--pattern":\n'
            "            pattern = args[i + 1]\n"
            '    if pattern.endswith(".tar.gz"):\n'
            "        sys.stdout.buffer.write(b'corrupted wrong bytes')\n"
            "    else:\n"
            '        fixture_dir = Path(os.environ["GH_STUB_FIXTURE_DIR"])\n'
            "        path = fixture_dir / pattern\n"
            "        if path.exists():\n"
            "            sys.stdout.buffer.write(path.read_bytes())\n"
            "    sys.exit(0)\n"
            "else:\n"
            "    args = sys.argv[1:]\n"
            '    pattern = ""\n'
            "    for i in range(len(args) - 1):\n"
            '        if args[i] == "--pattern":\n'
            "            pattern = args[i + 1]\n"
            '    fixture_dir = Path(os.environ["GH_STUB_FIXTURE_DIR"])\n'
            "    path = fixture_dir / pattern\n"
            "    if path.exists():\n"
            "        sys.stdout.buffer.write(path.read_bytes())\n"
            "        sys.exit(0)\n"
            '    sys.stderr.write(f"gh: pattern not found: {pattern}\\n")\n'
            "    sys.exit(1)\n"
        )
        gh_path.chmod(0o755)

    def write_config(self, repo: str = "https://github.com/fixture-owner/ffmpeg-patches") -> Path:
        """Write a temporary build-config.env pointing to the fixture."""
        config_path = self.temp_dir / "build-config.env"
        config_path.write_text(
            f'FFMPEG_FIX_SERIES_REPO="{repo}"\n'
            f'FFMPEG_FIX_SERIES_TAG="{shared_series.TAG}"\n'
            f'FFMPEG_FIX_SERIES_SHA256="{self.digest}"\n'
            'FFMPEG_TAG="n9.0.2"\n'
            'FFMPEG_COMMIT="commit-fixture"\n'
            'FFMPEG_FIX_SERIES_CACHE="none"\n'
            'FFMPEG_FIX_SERIES_VERIFY="sha256"\n'
        )
        return config_path

    def run_script(
        self,
        *args: str,
        config: Path | None = None,
        extra_env: dict[str, str] | None = None,
        include_gh: bool = True,
        bin_dir: Path | None = None,
    ) -> TextCommandResult:
        """Execute ffmpeg-shared-series.sh inside the isolated environment."""
        effective_bin = bin_dir or self.bin_dir
        if not include_gh:
            effective_bin = self.temp_dir / "bin_no_gh"
            if not effective_bin.exists():
                effective_bin.mkdir()
                for child in (bin_dir or self.bin_dir).iterdir():
                    if child.name != "gh":
                        (effective_bin / child.name).symlink_to(child)
        cfg = config or self.write_config()
        env = {
            "PATH": str(effective_bin),
            "BUILD_CONFIG": str(cfg),
            "GH_STUB_FIXTURE_DIR": str(self.fixture_dir),
            "GH_STUB_ARGV_FILE": str(self.temp_dir / "gh_argv.txt"),
            "LC_ALL": "C",
        }
        if extra_env:
            env.update(extra_env)
        bash = shutil.which("bash")
        self.assertIsNotNone(bash)
        assert bash is not None
        result = run_command(
            [bash, str(SCRIPT), *args],
            allowed_executables=(bash,),
            capture_output=True,
            text=True,
            env=env,
            check=False,
            timeout_seconds=60,
        )
        assert isinstance(result, TextCommandResult)
        return result

    def run_deadline(
        self,
        seconds: int | str,
        *cmd: str,
        path_dir: Path | None = None,
    ) -> TextCommandResult:
        """Run run_with_deadline helper directly with isolated PATH."""
        effective_bin = path_dir if path_dir is not None else self.bin_dir
        bash = shutil.which("bash")
        self.assertIsNotNone(bash)
        assert bash is not None
        env = {
            "PATH": str(effective_bin),
            "LC_ALL": "C",
        }
        result = run_command(
            [bash, str(SCRIPT), "_run_with_deadline", str(seconds), *cmd],
            allowed_executables=(bash,),
            capture_output=True,
            text=True,
            env=env,
            check=False,
            timeout_seconds=30,
        )
        assert isinstance(result, TextCommandResult)
        return result

    def test_positive_curl_fails_gh_fetches(self) -> None:
        """Verify gh fallback fetches both release assets when curl fails."""
        dest = self.temp_dir / "dest"
        res = self.run_script("fetch", str(dest), extra_env={"GITHUB_ACTIONS": "true"})
        self.assertEqual(res.returncode, 0)
        self.assertTrue((dest / "series.txt").exists())
        tarball_url = (
            f"https://github.com/fixture-owner/ffmpeg-patches/releases/download/"
            f"{shared_series.TAG}/ffmpeg-patches-{shared_series.TAG}.tar.gz"
        )
        sums_url = (
            f"https://github.com/fixture-owner/ffmpeg-patches/releases/download/"
            f"{shared_series.TAG}/SHA256SUMS"
        )
        self.assertIn(
            f"download failed: {tarball_url}; fetching it with gh release download", res.stderr
        )
        self.assertIn(
            f"download failed: {sums_url}; fetching it with gh release download", res.stderr
        )
        self.assertIn(f"fetched {tarball_url} with gh release download", res.stderr)
        self.assertIn(f"fetched {sums_url} with gh release download", res.stderr)
        self.assertIn(
            f"::warning title=Release download fallback::{tarball_url} failed; fetched with gh release download",
            res.stdout,
        )
        argv_file = self.temp_dir / "gh_argv.txt"
        self.assertTrue(argv_file.exists())
        argv_content = argv_file.read_text()
        self.assertIn(
            f"release download {shared_series.TAG} --repo fixture-owner/ffmpeg-patches",
            argv_content,
        )
        self.assertIn(f"--pattern ffmpeg-patches-{shared_series.TAG}.tar.gz", argv_content)
        self.assertIn("--pattern SHA256SUMS", argv_content)

    def test_positive_curl_ok_gh_never_called(self) -> None:
        """Verify gh is not invoked when curl successfully downloads."""
        dest = self.temp_dir / "dest"
        res = self.run_script("fetch", str(dest), extra_env={"CURL_STUB_MODE": "ok"})
        self.assertEqual(res.returncode, 0)
        self.assertTrue((dest / "series.txt").exists())
        self.assertNotIn("gh release download", res.stderr)
        argv_file = self.temp_dir / "gh_argv.txt"
        self.assertFalse(argv_file.exists())

    def test_negative_gh_serves_wrong_bytes(self) -> None:
        """Verify sha256 check still validates content retrieved via fallback."""
        dest = self.temp_dir / "dest"
        res = self.run_script("fetch", str(dest), extra_env={"GH_STUB_MODE": "wrong_bytes"})
        self.assertEqual(res.returncode, 1)
        self.assertIn("build-config.env pins", res.stderr)
        self.assertIn("fetching it with gh release download", res.stderr)
        self.assertFalse(dest.exists())

    def test_negative_gh_exits_1(self) -> None:
        """Verify failure exit and cleanup when gh release download fails."""
        dest = self.temp_dir / "dest"
        res = self.run_script("fetch", str(dest), extra_env={"GH_STUB_MODE": "fail"})
        self.assertEqual(res.returncode, 1)
        self.assertIn("download failed:", res.stderr)
        self.assertIn("gh release download failed", res.stderr)
        self.assertFalse(dest.exists())

    def test_negative_no_gh_on_path(self) -> None:
        """Verify clear error message when gh is missing on PATH."""
        dest = self.temp_dir / "dest"
        res = self.run_script("fetch", str(dest), include_gh=False)
        self.assertEqual(res.returncode, 1)
        self.assertIn("no gh on PATH for fallback", res.stderr)
        self.assertIn("download failed:", res.stderr)
        self.assertFalse(dest.exists())

    def test_negative_repo_host_other_than_github(self) -> None:
        """Verify non-github and lookalike host URLs bypass gh fallback."""
        hosts = [
            "https://gitlab.com/fixture-owner/ffmpeg-patches",
            "https://github.com.example/fixture-owner/ffmpeg-patches",
        ]
        for host in hosts:
            with self.subTest(host=host):
                config = self.write_config(repo=host)
                dest = self.temp_dir / f"dest_{abs(hash(host))}"
                res = self.run_script("fetch", str(dest), config=config)
                self.assertEqual(res.returncode, 1)
                self.assertIn(f"download failed: {host}", res.stderr)
                self.assertNotIn("fetching it with gh release download", res.stderr)
                argv_file = self.temp_dir / "gh_argv.txt"
                self.assertFalse(argv_file.exists())
                self.assertFalse(dest.exists())
        # http:// URL rejected without fallback
        dest_http = self.temp_dir / "dest_http"
        http_url = "http://github.com/fixture-owner/ffmpeg-patches/releases/download/v0.0.0-fixture/a.tar.gz"
        res_http = self.run_script(
            "fetch",
            str(dest_http),
            extra_env={"FFMPEG_FIX_SERIES_URL": http_url},
        )
        self.assertEqual(res_http.returncode, 2)
        self.assertIn("unsupported URL", res_http.stderr)
        self.assertNotIn("fetching it with gh release download", res_http.stderr)

    def test_boundary_oversize_asset(self) -> None:
        """Verify fallback aborts and cleans up when asset exceeds maximum size."""
        dest = self.temp_dir / "dest"
        res = self.run_script("fetch", str(dest), extra_env={"GH_STUB_MODE": "oversize"})
        self.assertEqual(res.returncode, 1)
        self.assertIn("exceeded maximum size", res.stderr)
        self.assertIn("download failed:", res.stderr)
        self.assertFalse(dest.exists())

    def test_boundary_gh_hangs_timeout(self) -> None:
        """Verify hanging gh call times out and leaves no orphan sleep processes."""
        dest = self.temp_dir / "dest"
        sleep_arg = "30.71829"
        start = time.monotonic()
        res = self.run_script(
            "fetch",
            str(dest),
            extra_env={
                "GH_STUB_MODE": "hang",
                "GH_STUB_SLEEP_ARG": sleep_arg,
                "FFMPEG_FIX_SERIES_DOWNLOAD_SECONDS": "2",
            },
        )
        elapsed = time.monotonic() - start
        self.assertEqual(res.returncode, 1)
        self.assertLess(elapsed, 20.0)
        self.assertIn("download failed:", res.stderr)
        self.assertFalse(dest.exists())
        with self.subTest("process cleanup"):
            pgrep = shutil.which("pgrep")
            if pgrep is None:
                self.skipTest("pgrep not on PATH")
            check = run_command(
                [pgrep, "-f", f"^sleep {sleep_arg}$"],
                allowed_executables=(pgrep,),
                capture_output=True,
                text=True,
                check=False,
                timeout_seconds=60,
            )
            self.assertEqual(check.returncode, 1)
            self.assertEqual(check.stdout, "")

    def test_positive_deadline_helper_stops_hanging_command(self) -> None:
        """Plant a hanging stub command and verify run_with_deadline stops it within bound."""
        start = time.monotonic()
        res = self.run_deadline(1, "sleep", "15")
        elapsed = time.monotonic() - start
        self.assertNotEqual(res.returncode, 0)
        self.assertLess(elapsed, 5.0)

    def test_negative_deadline_helper_preserves_command_exit_status(self) -> None:
        """Plant fast commands returning 0 and 42; verify run_with_deadline preserves status without timeout note."""
        res0 = self.run_deadline(5, "bash", "-c", "exit 0")
        self.assertEqual(res0.returncode, 0)
        self.assertNotIn("missing", res0.stderr)
        res42 = self.run_deadline(5, "bash", "-c", "exit 42")
        self.assertEqual(res42.returncode, 42)
        self.assertNotIn("missing", res42.stderr)

    def test_boundary_deadline_helper_perl_fallback(self) -> None:
        """Plant PATH with perl but without timeout and gtimeout; verify perl path stops a hanging stub."""
        bin_perl = self.temp_dir / "bin_perl"
        bin_perl.mkdir()
        for tool in ("bash", "dirname", "perl", "sleep", "kill", "python3"):
            path = shutil.which(tool)
            if path:
                (bin_perl / tool).symlink_to(path)
        start = time.monotonic()
        res = self.run_deadline(1, "sleep", "15", path_dir=bin_perl)
        elapsed = time.monotonic() - start
        self.assertNotEqual(res.returncode, 0)
        self.assertLess(elapsed, 5.0)
        self.assertIn("deadline helper: using perl", res.stderr)

    def test_boundary_deadline_helper_missing_all_tools(self) -> None:
        """Plant PATH lacking timeout, gtimeout, and perl; verify run_with_deadline fails closed with missing-tools note."""
        bin_none = self.temp_dir / "bin_none"
        bin_none.mkdir()
        for tool in ("bash", "dirname", "sleep"):
            path = shutil.which(tool)
            if path:
                (bin_none / tool).symlink_to(path)
        res = self.run_deadline(1, "sleep", "0.1", path_dir=bin_none)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("deadline helper: timeout, gtimeout, and perl are missing", res.stderr)

    def test_positive_wget_timeout_reaches_fallback(self) -> None:
        """Plant a stub wget that sleeps past S; verify direct download stops within bound and reaches forge API fallback."""
        bin_wget = self.temp_dir / "bin_wget"
        bin_wget.mkdir()
        for child in self.bin_dir.iterdir():
            if child.name != "curl":
                (bin_wget / child.name).symlink_to(child)
        wget_stub = bin_wget / "wget"
        wget_stub.write_text(
            "#!/usr/bin/env python3\n"
            '"""Stub wget that hangs beyond deadline."""\n'
            "import os, sys\n"
            'os.execvp("sleep", ["sleep", "30"])\n'
        )
        wget_stub.chmod(0o755)

        dest = self.temp_dir / "dest_wget"
        start = time.monotonic()
        res = self.run_script(
            "fetch",
            str(dest),
            bin_dir=bin_wget,
            extra_env={"FFMPEG_FIX_SERIES_DOWNLOAD_SECONDS": "2"},
        )
        elapsed = time.monotonic() - start
        self.assertEqual(res.returncode, 0)
        self.assertLess(elapsed, 15.0)
        self.assertTrue((dest / "series.txt").exists())
        self.assertIn("fetching it with gh release download", res.stderr)
        self.assertIn("fetched", res.stderr)

    def test_boundary_download_seconds_invalid(self) -> None:
        """Verify invalid FFMPEG_FIX_SERIES_DOWNLOAD_SECONDS values exit code 2."""
        dest = self.temp_dir / "dest"
        for val in ("0", "abc", "1e3"):
            with self.subTest(val=val):
                res = self.run_script(
                    "fetch",
                    str(dest),
                    extra_env={"FFMPEG_FIX_SERIES_DOWNLOAD_SECONDS": val},
                )
                self.assertEqual(res.returncode, 2)
                self.assertIn("FFMPEG_FIX_SERIES_DOWNLOAD_SECONDS", res.stderr)


class SharedSeriesWrapperTimeoutTest(unittest.TestCase):
    """Verify shared-series wrapper timeout derivation in ffmpeg_patch_stack.py."""

    def test_timeout_derivation_unset_uses_default(self) -> None:
        """Plant unset FFMPEG_FIX_SERIES_DOWNLOAD_SECONDS and expect default 600 s."""
        self.assertEqual(shared_series_timeout_seconds({}), 600)

    def test_timeout_derivation_standard_s_120(self) -> None:
        """Plant S=120 and expect max(600, N * 2 * 120 + 120) with N = SHARED_SERIES_MAX_DOWNLOADS."""
        expected = max(600, SHARED_SERIES_MAX_DOWNLOADS * 2 * 120 + 120)
        self.assertEqual(
            shared_series_timeout_seconds({"FFMPEG_FIX_SERIES_DOWNLOAD_SECONDS": "120"}),
            expected,
        )

    def test_timeout_derivation_extreme_s_999999(self) -> None:
        """Plant S=999999 and expect N * 2 * 999999 + 120."""
        expected = SHARED_SERIES_MAX_DOWNLOADS * 2 * 999999 + 120
        self.assertEqual(
            shared_series_timeout_seconds({"FFMPEG_FIX_SERIES_DOWNLOAD_SECONDS": "999999"}),
            expected,
        )

    def test_timeout_derivation_invalid_strings_revert_to_default(self) -> None:
        """Plant invalid values ('0', 'abc', '-1', '1e3', '1000000', '') and expect default 600 s."""
        for invalid in ("0", "abc", "-1", "1e3", "1000000", ""):
            with self.subTest(invalid=invalid):
                self.assertEqual(
                    shared_series_timeout_seconds({"FFMPEG_FIX_SERIES_DOWNLOAD_SECONDS": invalid}),
                    600,
                )


if __name__ == "__main__":
    unittest.main()
