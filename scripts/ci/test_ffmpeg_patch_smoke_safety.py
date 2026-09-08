"""Exercise the real smoke Git operations without a native FFmpeg build."""

# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SMOKE = ROOT / "ffmpeg-patches/test/build-and-run.sh"


class SmokeSafety(unittest.TestCase):
    def git(self, path: Path, *args: str) -> str:
        return subprocess.check_output(  # noqa: S603 -- fixed Git argv and fixture-owned paths
            [
                shutil.which("git") or "/usr/bin/git",
                "-c",
                "core.hooksPath=/dev/null",
                "-c",
                "commit.gpgsign=false",
                "-c",
                "user.name=Smoke Fixture",
                "-c",
                "user.email=fixture@example.invalid",
                "-C",
                str(path),
                *args,
            ],
            text=True,
            stderr=subprocess.DEVNULL,
        )

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="ffmpeg-smoke-safety-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.upstream = self.root / "upstream"
        self.caller = self.root / "caller"
        for path in (self.upstream, self.caller):
            path.mkdir()
            self.git(path, "init", "-q", "-b", "master")
            (path / "sample").write_text("before\n")
            self.git(path, "add", "sample")
            self.git(path, "commit", "-qm", "initial fixture")
        configure = self.upstream / "configure"
        configure.write_text(
            "#!/bin/sh\ncat > ffmpeg <<'BINARY'\n#!/bin/sh\n"
            'case "$*" in\n*filter=libvmaf*) echo tiny_model;;\n'
            "*filter=vmaf_pre*) exit 0;;\n*) exit 1;;\nesac\nBINARY\n"
            "chmod +x ffmpeg\n"
        )
        configure.chmod(0o755)
        self.git(self.upstream, "add", "configure")
        self.git(self.upstream, "commit", "-qm", "fake native configure")
        self.git(self.upstream, "tag", "n9.0.1")
        (self.upstream / "sample").write_text("patched\n")
        self.git(self.upstream, "commit", "-qam", "integration patch")
        patch = self.git(self.upstream, "format-patch", "-1", "--stdout")
        self.project = self.root / "fixture"
        patches = self.project / "ffmpeg-patches"
        (patches / "test").mkdir(parents=True)
        self.script = patches / "test/build-and-run.sh"
        shutil.copy2(SMOKE, self.script)
        (patches / "0001-fixture.patch").write_text(patch)
        (patches / "series.txt").write_text("0001-fixture.patch\n")
        (self.project / "build-config.env").write_text(
            f'FFMPEG_REMOTE="{self.upstream}"\nFFMPEG_TAG="n9.0.1"\n'
        )
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for name, body in (("pkg-config", "exit 0"), ("make", "exit 0"), ("nproc", "echo 1")):
            path = self.bin / name
            path.write_text("#!/bin/sh\n" + body + "\n")
            path.chmod(0o755)
        (self.caller / "unique-staged-work").write_text("preserve this staging\n")
        self.git(self.caller, "add", "unique-staged-work")
        self.before_head = self.git(self.caller, "rev-parse", "HEAD")
        self.before_index = (self.caller / ".git/index").read_bytes()
        self.checkout = self.root / "ffmpeg-checkout"
        self.env = {
            **os.environ,
            "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
            "FFMPEG_SRC": str(self.checkout),
            "KEEP_BUILD": "1",
        }

    def run_smoke(self) -> subprocess.CompletedProcess[str]:
        # Fixed Bash executable, copied repository script and fixture-owned cwd.
        return subprocess.run(  # noqa: S603
            [shutil.which("bash") or "/bin/bash", str(self.script)],
            cwd=self.project,
            env=self.env,
            text=True,
            capture_output=True,
            timeout=30,
        )

    def assert_caller_preserved(self) -> None:
        self.assertEqual(self.git(self.caller, "rev-parse", "HEAD"), self.before_head)
        self.assertEqual((self.caller / ".git/index").read_bytes(), self.before_index)
        self.assertEqual((self.caller / "sample").read_text(), "before\n")
        self.assertEqual(
            (self.caller / "unique-staged-work").read_text(), "preserve this staging\n"
        )
        self.git(self.caller, "diff", "--cached")

    def test_index_only_hook_environment_cannot_overwrite_caller_staging(self) -> None:
        self.env["GIT_INDEX_FILE"] = str(self.caller / ".git/index")
        result = self.run_smoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_caller_preserved()
        self.assertEqual((self.checkout / "sample").read_text(), "patched\n")

    def test_all_hook_variables_and_global_hooks_are_isolated(self) -> None:
        hooks = self.root / "hostile-hooks"
        hooks.mkdir()
        marker = self.root / "caller-hook-ran"
        for name in ("post-checkout", "applypatch-msg", "pre-applypatch", "post-applypatch"):
            path = hooks / name
            path.write_text(f'#!/bin/sh\necho executed > "{marker}"\n')
            path.chmod(0o755)
        config = self.root / "hostile-gitconfig"
        config.write_text(f"[core]\n hooksPath = {hooks}\n[commit]\n gpgSign = true\n")
        self.env.update(
            GIT_DIR=str(self.caller / ".git"),
            GIT_WORK_TREE=str(self.caller),
            GIT_COMMON_DIR=str(self.caller / ".git"),
            GIT_INDEX_FILE=str(self.caller / ".git/index"),
            GIT_CONFIG_GLOBAL=str(config),
        )
        result = self.run_smoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_caller_preserved()
        self.assertFalse(marker.exists())

    def test_existing_source_is_refused_without_touching_caller(self) -> None:
        self.checkout.mkdir()
        marker = self.checkout / "valuable-output"
        marker.write_text("preserve\n")
        result = self.run_smoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(marker.read_text(), "preserve\n")
        self.assert_caller_preserved()

    def test_development_override_is_refused_before_clone(self) -> None:
        self.env["FFMPEG_SHA"] = "master"
        result = self.run_smoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.checkout.exists(), result.stdout + result.stderr)
        self.assert_caller_preserved()


if __name__ == "__main__":
    unittest.main()
