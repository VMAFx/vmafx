# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for scripts/ci/ffmpeg-patches-surface-check.sh (Issue #2435, ADR-2166)."""

from __future__ import annotations

import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]  # PyYAML ships no stubs in the hook env, as in test_clang_format_scope.py

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

SCRIPT = ROOT / "scripts" / "ci" / "ffmpeg-patches-surface-check.sh"
HOOK_ID = "ffmpeg-patches-surface-docs"
SURFACE_ONLY_DONE_LINE = "--surface-only: tree checks passed"
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


def _run_script(script: Path, cwd: Path, *args: str, **extra: str) -> TextCommandResult:
    """Run the surface check script with bash, the only executable allowed."""
    bash = _tool("bash")
    return run_command(
        [bash, str(script), *args],
        allowed_executables=[bash],
        cwd=str(cwd),
        capture_output=True,
        text=True,
        env=_clean_env(**extra),
        timeout_seconds=SCRIPT_TIMEOUT_S,
    )


class _FixtureTree:
    """Build the temporary fixture repository both test classes run the script in."""

    def _prepare_temp_worktree(self, temp_dir: Path, *, origin_master: bool = True) -> Path:
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
        if origin_master:
            _git(temp_dir, "update-ref", "refs/remotes/origin/master", "HEAD")
        return temp_dir


class FfmpegPatchesSurfaceCheckTests(_FixtureTree, unittest.TestCase):
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
        self.assertRegex(res.stdout, r"across [1-9][0-9]* scanned doc files")

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


class SurfaceOnlyModeTests(_FixtureTree, unittest.TestCase):
    """Verify the --surface-only mode the pre-commit hook runs."""

    def test_surface_only_passes_without_origin_master(self) -> None:
        """A checkout without origin/master passes the tree checks; the full gate cannot."""
        with tempfile.TemporaryDirectory() as td:
            tmp = self._prepare_temp_worktree(Path(td), origin_master=False)
            script = tmp / "scripts" / "ci" / SCRIPT.name

            res = _run_script(script, tmp, "--surface-only", REPO_ROOT=str(tmp))
            self.assertEqual(res.returncode, 0, f"stdout:\n{res.stdout}\nstderr:\n{res.stderr}")
            self.assertIn(SURFACE_ONLY_DONE_LINE, res.stdout)
            self.assertNotIn("PR body from", res.stdout)

            full = _run_script(script, tmp, REPO_ROOT=str(tmp))
            self.assertEqual(full.returncode, 2, f"stderr:\n{full.stderr}")
            self.assertIn("origin/master not found", full.stderr)

    def test_surface_only_fails_on_planted_doc(self) -> None:
        """A retired filter in a doc fails the tree checks in --surface-only mode too."""
        with tempfile.TemporaryDirectory() as td:
            tmp = self._prepare_temp_worktree(Path(td), origin_master=False)
            target_doc = tmp / "docs" / "usage" / "vmaf-tune-ffmpeg.md"
            with target_doc.open("a", encoding="utf-8") as fp:
                fp.write("\nPlanted defect: use vmaf_pre in the graph.\n")

            res = _run_script(
                tmp / "scripts" / "ci" / SCRIPT.name, tmp, "--surface-only", REPO_ROOT=str(tmp)
            )
            self.assertEqual(res.returncode, 1, f"stderr:\n{res.stderr}")
            self.assertIn("retired filter/option in doc", res.stderr)
            self.assertNotIn(SURFACE_ONLY_DONE_LINE, res.stdout)

    def test_surface_only_fails_on_planted_patch_test(self) -> None:
        """Planted libvmaf_sycl= in patch test fails in --surface-only mode."""
        with tempfile.TemporaryDirectory() as td:
            tmp = self._prepare_temp_worktree(Path(td), origin_master=False)
            target_script = tmp / "ffmpeg-patches" / "test" / "check-sycl-feature-routing.sh"
            with target_script.open("a", encoding="utf-8") as fp:
                fp.write(
                    "\n# planted defect:\n# -filter_complex '[0:v][1:v]libvmaf_sycl=threads=1'\n"
                )

            res = _run_script(
                tmp / "scripts" / "ci" / SCRIPT.name, tmp, "--surface-only", REPO_ROOT=str(tmp)
            )
            self.assertEqual(res.returncode, 1, f"stderr:\n{res.stderr}")
            self.assertIn("patch test carries retired filter/option", res.stderr)
            self.assertNotIn(SURFACE_ONLY_DONE_LINE, res.stdout)

    def test_surface_only_fails_when_zero_docs_scanned(self) -> None:
        """Zero doc files scanned in fixture tree fails surface check."""
        with tempfile.TemporaryDirectory() as td:
            tmp = self._prepare_temp_worktree(Path(td), origin_master=False)
            shutil.rmtree(tmp / "docs", ignore_errors=True)
            (tmp / "README.md").unlink(missing_ok=True)
            (tmp / "ffmpeg-patches" / "README.md").unlink(missing_ok=True)
            _git(tmp, "add", "-A")
            _git(tmp, "commit", "-q", "-m", "remove docs")

            res = _run_script(
                tmp / "scripts" / "ci" / SCRIPT.name, tmp, "--surface-only", REPO_ROOT=str(tmp)
            )
            self.assertEqual(res.returncode, 1, f"stderr:\n{res.stderr}")
            self.assertIn("zero doc files scanned", res.stderr)
            self.assertNotIn(SURFACE_ONLY_DONE_LINE, res.stdout)

    def test_surface_only_fails_when_doc_listing_fails(self) -> None:
        """A git ls-files failure (corrupt index) is named, never read as an empty doc list."""
        with tempfile.TemporaryDirectory() as td:
            tmp = self._prepare_temp_worktree(Path(td), origin_master=False)
            (tmp / ".git" / "index").write_bytes(b"not an index")

            res = _run_script(
                tmp / "scripts" / "ci" / SCRIPT.name, tmp, "--surface-only", REPO_ROOT=str(tmp)
            )
            self.assertEqual(res.returncode, 1, f"stderr:\n{res.stderr}")
            self.assertIn("[FAIL] cannot list docs", res.stderr)
            self.assertNotIn("zero doc files scanned", res.stderr)
            self.assertNotIn(SURFACE_ONLY_DONE_LINE, res.stdout)

    def test_bash_32_contract_no_mapfile_or_readarray(self) -> None:
        """Script must not use mapfile or readarray because macOS bash 3.2 lacks them."""
        content = SCRIPT.read_text(encoding="utf-8")
        for lineno, line in enumerate(content.splitlines(), start=1):
            code_only = line.split("#", 1)[0]
            for token in ("mapfile", "readarray"):
                self.assertNotIn(
                    token,
                    code_only,
                    f"Line {lineno} violates bash 3.2 compatibility by using '{token}': {line}",
                )

    def test_bash_32_contract_array_loops_survive_empty_arrays(self) -> None:
        """Bash 3.2 under set -u aborts on "${a[@]}" when a is empty, so every loop is guarded."""
        content = SCRIPT.read_text(encoding="utf-8")
        loops = re.findall(r'for \w+ in "\$\{(\w+)\[@\]\}"', content)
        self.assertTrue(loops, "no array loops found; the pattern no longer matches the script")
        for name in loops:
            count_guard = f'"${{#{name}[@]}}" -eq 0'
            # A quoted first element followed by whitespace or ")" is a literal, never a glob.
            literal_list = re.search(rf'\b{name}=\(\s*"[^"]*"(?=[\s)])', content)
            self.assertTrue(
                count_guard in content or literal_list,
                f'loop over "${{{name}[@]}}" has no empty-array guard; use ${{{name}[@]+"${{{name}[@]}}"}}',
            )

    def test_bad_arguments_exit_two(self) -> None:
        """An unknown option or a second argument is a usage error, never a pass."""
        cases = {
            "unknown option": ("--surface",),
            "second argument": ("--surface-only", "--surface-only"),
        }
        for label, args in cases.items():
            with self.subTest(label):
                res = _run_script(SCRIPT, ROOT, *args)
                self.assertEqual(res.returncode, 2, f"stderr:\n{res.stderr}")
                self.assertIn("the only option is --surface-only", res.stderr)
                self.assertNotIn("[PASS]", res.stdout)

    def test_hook_runs_surface_only_on_every_tree_check_input(self) -> None:
        """The hook runs --surface-only, and its files pattern covers every path the checks read."""
        config = yaml.safe_load((ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8"))
        hooks = [
            hook
            for repo in config.get("repos") or []
            for hook in repo.get("hooks") or []
            if hook.get("id") == HOOK_ID
        ]
        self.assertEqual(len(hooks), 1, f"expected one {HOOK_ID} hook")
        hook = hooks[0]
        self.assertEqual(
            hook["entry"], "bash scripts/ci/ffmpeg-patches-surface-check.sh --surface-only"
        )
        files = re.compile(hook["files"])

        body = SCRIPT.read_text(encoding="utf-8")
        block = re.search(r"local configs=\(\n(.*?)\n\s*\)", body, re.S)
        self.assertIsNotNone(block, "configs=( ... ) list not found in the script")
        assert block is not None
        configs = re.findall(r'"([^"]+)"', block.group(1))
        self.assertGreaterEqual(len(configs), 9)
        read_paths = [
            *configs,
            "README.md",
            "docs/usage/ffmpeg.md",
            "ffmpeg-patches/README.md",
            "ffmpeg-patches/0001-example.patch",
            "ffmpeg-patches/test/build-and-run.sh",
            "scripts/ci/ffmpeg-patches-surface-check.sh",
        ]
        for path in read_paths:
            with self.subTest(read=path):
                self.assertIsNotNone(files.search(path), f"{path} does not trigger {HOOK_ID}")
        for path in ("core/src/libvmaf.c", "docs/usage/ffmpeg.txt", "Dockerfile.node"):
            with self.subTest(unread=path):
                self.assertIsNone(files.search(path), f"{path} triggers {HOOK_ID}")


if __name__ == "__main__":
    unittest.main()
