# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for scripts/ci/ffmpeg-patches-surface-check.sh (Issue #2435, ADR-2166)."""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

SCRIPT = ROOT / "scripts" / "ci" / "ffmpeg-patches-surface-check.sh"
GIT_TIMEOUT_S = 60
SCRIPT_TIMEOUT_S = 300


def _clean_env(**extra: str) -> dict[str, str]:
    """Return the caller environment without GIT_* and with a fixture-only identity."""
    env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    env.update(
        GIT_CONFIG_GLOBAL=os.devnull,
        GIT_CONFIG_SYSTEM=os.devnull,
        GIT_AUTHOR_NAME="t",
        GIT_AUTHOR_EMAIL="t@example.invalid",
        GIT_COMMITTER_NAME="t",
        GIT_COMMITTER_EMAIL="t@example.invalid",
    )
    env.update(extra)
    return env


def _tool(name: str) -> str:
    """Return the absolute path of a tool, or skip when it is absent."""
    path = shutil.which(name)
    if path is None:
        raise unittest.SkipTest(f"{name} is not on PATH")
    return path


def _git(repo: Path, *args: str) -> None:
    """Run git in a fixture repository with the clean environment."""
    git = _tool("git")
    run_command(
        [git, "-C", str(repo), *args],
        allowed_executables=[git],
        check=True,
        env=_clean_env(),
        capture_output=True,
        timeout_seconds=GIT_TIMEOUT_S,
    )


def _run_script(script: Path, cwd: Path, **extra: str) -> TextCommandResult:
    """Run the surface check script with bash, the only executable allowed."""
    bash = _tool("bash")
    return run_command(
        [bash, str(script)],
        allowed_executables=[bash],
        cwd=str(cwd),
        capture_output=True,
        text=True,
        env=_clean_env(**extra),
        timeout_seconds=SCRIPT_TIMEOUT_S,
    )


class FfmpegPatchesSurfaceCheckTests(unittest.TestCase):
    """Verify surface invariant checks on series, build configs, and docs."""

    def test_positive_on_clean_tree(self) -> None:
        """On clean worktree, surface checks and public-surface gate pass."""
        res = _run_script(SCRIPT, ROOT)
        self.assertEqual(
            res.returncode,
            0,
            f"Expected exit 0 on clean tree, got {res.returncode}.\nstdout:\n{res.stdout}\nstderr:\n{res.stderr}",
        )
        self.assertIn("series surface: zero vmaf-named filters/options", res.stdout)
        self.assertIn("fork build configs: zero retired filters/options", res.stdout)
        self.assertIn("docs examples: zero retired filters/options", res.stdout)
        self.assertIn("patch tests: zero retired filters/options", res.stdout)

    def _prepare_temp_worktree(self, temp_dir: Path) -> Path:
        """Stage a minimal fixture tree with the three surfaces and script."""
        # Copy ffmpeg-patches
        shutil.copytree(ROOT / "ffmpeg-patches", temp_dir / "ffmpeg-patches")
        # Copy scripts/ci needed by script
        (temp_dir / "scripts" / "ci").mkdir(parents=True, exist_ok=True)
        shutil.copy(SCRIPT, temp_dir / "scripts" / "ci" / SCRIPT.name)
        for helper in ("pr-body-input.sh", "pr-diff-base.sh"):
            src = ROOT / "scripts" / "ci" / helper
            if src.is_file():
                shutil.copy(src, temp_dir / "scripts" / "ci" / helper)
        # Copy allow-listed build configs
        for rel in (
            "Containerfile.vmafx",
            "Dockerfile",
            "Dockerfile.ffmpeg",
            "dev/Containerfile",
            "docker/Dockerfile.node",
            "scripts/test/sycl-dev-container.sh",
            ".github/workflows/ffmpeg-patch-stack.yml",
            "scripts/ci/tidy-integrations-build.sh",
        ):
            src = ROOT / rel
            if src.is_file():
                dst = temp_dir / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy(src, dst)
        # Copy allow-listed doc files
        for rel in (
            "README.md",
            "docs/reference/faq.md",
            "docs/adr/0000-template.md",
            "docs/usage/ffmpeg.md",
            "docs/usage/vmaf-tune-ffmpeg.md",
            "docs/usage/vmaf-tune-codec-adapters.md",
            "docs/usage/vmaf-vpl.md",
            "docs/development/sycl-zerocopy-testing.md",
            "docs/ai/inference.md",
            "docs/ai/model-registry.md",
            "docs/ai/roadmap.md",
            "docs/ai/models/fastdvdnet_pre.md",
            "docs/ai/models/learned_filter_v1.md",
            "docs/api/dnn.md",
            "docs/api/models-and-features.md",
        ):
            src = ROOT / rel
            if src.is_file():
                dst = temp_dir / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy(src, dst)

        # A fixture repository so origin/master exists for diff base resolution.
        _git(temp_dir, "init", "-q", "-b", "master")
        _git(temp_dir, "add", ".")
        _git(temp_dir, "commit", "-q", "-m", "init")
        _git(temp_dir, "update-ref", "refs/remotes/origin/master", "HEAD")
        return temp_dir

    def test_planted_negative_series_surface(self) -> None:
        """Planted vmaf_pre in a patch causes series surface check failure."""
        with tempfile.TemporaryDirectory() as td:
            tmp = self._prepare_temp_worktree(Path(td))
            target_patch = tmp / "ffmpeg-patches" / "0005-add-vmafx-pre-filter.patch"
            with target_patch.open("a", encoding="utf-8") as fp:
                fp.write("\n+/* planted defect: vmaf_pre filter */\n")

            res = _run_script(tmp / "scripts" / "ci" / SCRIPT.name, tmp, REPO_ROOT=str(tmp))
            self.assertNotEqual(res.returncode, 0, "Expected non-zero exit code")
            self.assertIn("patch carries retired vmaf filter/option", res.stderr)

    def test_planted_negative_fork_build_config_surface(self) -> None:
        """Planted vmaf_pre in a build config causes config check failure."""
        with tempfile.TemporaryDirectory() as td:
            tmp = self._prepare_temp_worktree(Path(td))
            target_cfg = tmp / "Containerfile.vmafx"
            with target_cfg.open("a", encoding="utf-8") as fp:
                fp.write("\n# planted defect: enable vmaf_pre filter\n")

            res = _run_script(tmp / "scripts" / "ci" / SCRIPT.name, tmp, REPO_ROOT=str(tmp))
            self.assertNotEqual(res.returncode, 0, "Expected non-zero exit code")
            self.assertIn("fork build config references retired filter/option", res.stderr)

    def test_planted_negative_docs_examples_surface(self) -> None:
        """Planted vmaf_pre in docs outside migration guide causes doc check failure."""
        with tempfile.TemporaryDirectory() as td:
            tmp = self._prepare_temp_worktree(Path(td))
            target_doc = tmp / "docs" / "usage" / "vmaf-tune-ffmpeg.md"
            with target_doc.open("a", encoding="utf-8") as fp:
                fp.write("\nPlanted defect: use vmaf_pre in the graph.\n")

            res = _run_script(tmp / "scripts" / "ci" / SCRIPT.name, tmp, REPO_ROOT=str(tmp))
            self.assertNotEqual(res.returncode, 0, "Expected non-zero exit code")
            self.assertIn("retired filter/option in doc", res.stderr)

    def test_planted_doc_outside_old_list_fails(self) -> None:
        """Planted libvmaf_cuda in a doc outside old allow-list fails check."""
        with tempfile.TemporaryDirectory() as td:
            tmp = self._prepare_temp_worktree(Path(td))
            target_doc = tmp / "docs" / "reference" / "faq.md"
            with target_doc.open("a", encoding="utf-8") as fp:
                fp.write("\nPlanted defect: run libvmaf_cuda filter here.\n")

            res = _run_script(tmp / "scripts" / "ci" / SCRIPT.name, tmp, REPO_ROOT=str(tmp))
            self.assertNotEqual(res.returncode, 0, "Expected non-zero exit code")
            self.assertIn("retired filter/option in doc", res.stderr)

    def test_planted_doc_in_adr_passes(self) -> None:
        """Planted libvmaf_cuda in docs/adr is excluded and passes check."""
        with tempfile.TemporaryDirectory() as td:
            tmp = self._prepare_temp_worktree(Path(td))
            target_adr = tmp / "docs" / "adr" / "0000-template.md"
            with target_adr.open("a", encoding="utf-8") as fp:
                fp.write("\nHistorical context: libvmaf_cuda filter was used previously.\n")

            res = _run_script(tmp / "scripts" / "ci" / SCRIPT.name, tmp, REPO_ROOT=str(tmp))
            self.assertEqual(
                res.returncode,
                0,
                f"Expected exit 0 for docs/adr, got {res.returncode}.\nstderr:\n{res.stderr}",
            )
            self.assertIn("docs examples: zero retired filters/options", res.stdout)

    def test_planted_negative_patch_test_surface(self) -> None:
        """Planted libvmaf_sycl= filter in a patch test script fails surface check."""
        with tempfile.TemporaryDirectory() as td:
            tmp = self._prepare_temp_worktree(Path(td))
            target_script = tmp / "ffmpeg-patches" / "test" / "check-sycl-feature-routing.sh"
            with target_script.open("a", encoding="utf-8") as fp:
                fp.write(
                    "\n# planted defect:\n# -filter_complex '[0:v][1:v]libvmaf_sycl=threads=1'\n"
                )

            res = _run_script(tmp / "scripts" / "ci" / SCRIPT.name, tmp, REPO_ROOT=str(tmp))
            self.assertNotEqual(res.returncode, 0, "Expected non-zero exit code")
            self.assertIn("patch test carries retired filter/option", res.stderr)


if __name__ == "__main__":
    unittest.main()
