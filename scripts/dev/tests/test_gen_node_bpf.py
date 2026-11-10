# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/dev/gen-node-bpf.sh against a fake root, fake clang and fake go (ADR-1622).

The script is the one place the node's eBPF object is generated. Each case
copies it into a disposable repository root, so the tool checks, the pin check
and the digest check run for real while `go generate` is a stub that writes a
known object. No clang, libbpf or network is needed except the libbpf header
file the script checks for, which the cases that reach it skip without.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / "scripts" / "dev" / "gen-node-bpf.sh"
PIN = "19.1.7"
OBJECT = b"fake bpf object\n"
OBJECT_SHA = hashlib.sha256(OBJECT).hexdigest()
GIT = shutil.which("git") or "git"
GIT_TIMEOUT_S = 60
SCRIPT_TIMEOUT_S = 60
HAS_LIBBPF = Path("/usr/include/bpf/bpf_helpers.h").is_file()

FAKE_CLANG = """#!/bin/sh
case "$1" in
  --version) echo "clang version {version}"; echo "Target: x86_64-pc-linux-gnu" ;;
  -print-targets) printf '  Registered Targets:\\n    bpf        - BPF (host endian)\\n' ;;
esac
"""
# Stands for `go generate`: counts the calls and writes the object and binding.
FAKE_GO = """#!/bin/sh
echo run >> "{counter}"
printf 'fake bpf object\\n' > cmd/vmafx-node/bpf/rclonebypass_bpfel.o
echo "${{FAKE_BINDING:-package bpf}}" > cmd/vmafx-node/bpf/rclonebypass_bpfel.go
"""


def executable(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class GenNodeBpf(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="gen-node-bpf-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.root = self.tmp / "root"
        (self.root / "scripts" / "dev").mkdir(parents=True)
        (self.root / "cmd" / "vmafx-node" / "bpf").mkdir(parents=True)
        shutil.copy(SCRIPT, self.root / "scripts" / "dev" / "gen-node-bpf.sh")
        for name in (
            "rclone_bypass.bpf.c",
            "vmlinux.h",
            "gen.go",
            "add_spdx_header.sh",
            "embed_generated_object.sh",
        ):
            (self.root / "cmd" / "vmafx-node" / "bpf" / name).write_text(name, encoding="utf-8")
        (self.root / "cmd" / "vmafx-node" / "bpf" / "rclonebypass_bpfel.go").write_text(
            "package bpf\n", encoding="utf-8"
        )
        (self.root / "go.mod").write_text("module example\n", encoding="utf-8")
        self.write_config(OBJECT_SHA)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        self.counter = self.tmp / "go-calls"
        executable(self.bin / "go", FAKE_GO.format(counter=self.counter))
        executable(self.bin / "llvm-strip", "#!/bin/sh\n")
        self.set_clang(PIN)

    def write_config(self, sha: str) -> None:
        (self.root / "build-config.env").write_text(
            f'BPF_CLANG_VERSION="{PIN}"\nBPF_OBJECT_SHA256="{sha}"\n', encoding="utf-8"
        )

    def set_clang(self, version: str) -> None:
        executable(self.bin / "clang", FAKE_CLANG.format(version=version))

    def run_script(
        self, *args: str, path: str | None = None, binding: str | None = None
    ) -> subprocess.CompletedProcess[str]:
        env = {
            "PATH": path if path is not None else f"{self.bin}:/usr/bin:/bin",
            "HOME": str(self.tmp),
        }
        if binding is not None:
            env["FAKE_BINDING"] = binding
        return subprocess.run(  # noqa: S603 - fixed argv, disposable root
            ["/usr/bin/env", "bash", str(self.root / "scripts" / "dev" / "gen-node-bpf.sh"), *args],
            capture_output=True,
            text=True,
            env=env,
            check=False,
            timeout=60,
        )

    def calls(self) -> int:
        return len(self.counter.read_text().split()) if self.counter.exists() else 0

    def test_missing_clang_fails_and_names_the_tool_and_the_pin(self) -> None:
        # Only the utilities the script runs: no clang, no clang-19.
        tools = self.tmp / "tools"
        tools.mkdir()
        for name in (
            "sed",
            "head",
            "cut",
            "grep",
            "sha256sum",
            "cat",
            "dirname",
            "env",
            "bash",
            "rm",
            "go",
        ):
            source = str(self.bin / name) if name == "go" else shutil.which(name)
            if source is None:
                self.fail(f"{name} not on PATH")
            (tools / name).symlink_to(source)
        result = self.run_script(path=str(tools))
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("clang not found", result.stderr)
        self.assertIn(PIN, result.stderr)
        self.assertIn("clang-19", result.stderr)
        self.assertEqual(self.calls(), 0)

    def test_unknown_argument_is_refused(self) -> None:
        result = self.run_script("--bogus")
        self.assertEqual(result.returncode, 64, result.stderr)

    @unittest.skipUnless(HAS_LIBBPF, "needs /usr/include/bpf/bpf_helpers.h (libbpf-dev)")
    def test_pinned_clang_and_matching_digest_generate(self) -> None:
        result = self.run_script("--require-pin")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(OBJECT_SHA, result.stdout)
        self.assertEqual(self.calls(), 1)

    @unittest.skipUnless(HAS_LIBBPF, "needs /usr/include/bpf/bpf_helpers.h (libbpf-dev)")
    def test_digest_mismatch_with_the_pinned_clang_fails(self) -> None:
        self.write_config("0" * 64)
        result = self.run_script("--require-pin")
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn("BPF_OBJECT_SHA256", result.stderr)

    @unittest.skipUnless(HAS_LIBBPF, "needs /usr/include/bpf/bpf_helpers.h (libbpf-dev)")
    def test_versioned_pinned_clang_wins_over_a_newer_default_clang(self) -> None:
        # The hosted Ubuntu runner has clang 21 as `clang` and the pin as `clang-19`.
        self.set_clang("21.1.8")
        executable(self.bin / "clang-19", FAKE_CLANG.format(version=PIN))
        result = self.run_script("--require-pin")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"clang {PIN} (pin {PIN})", result.stdout)
        self.assertNotIn("21.1.8", result.stderr)

    @unittest.skipUnless(HAS_LIBBPF, "needs /usr/include/bpf/bpf_helpers.h (libbpf-dev)")
    def test_other_clang_is_refused_with_require_pin(self) -> None:
        self.set_clang("18.1.8")
        result = self.run_script("--require-pin")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("18.1.8", result.stderr)
        self.assertIn(PIN, result.stderr)
        self.assertEqual(self.calls(), 0)

    @unittest.skipUnless(HAS_LIBBPF, "needs /usr/include/bpf/bpf_helpers.h (libbpf-dev)")
    def test_other_clang_without_require_pin_names_the_substitution(self) -> None:
        self.set_clang("23.1.1")
        self.write_config("0" * 64)  # a digest is only checked for the pinned clang
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("23.1.1", result.stderr)
        self.assertIn("not byte-identical", result.stderr)

    @unittest.skipUnless(HAS_LIBBPF, "needs /usr/include/bpf/bpf_helpers.h (libbpf-dev)")
    def test_up_to_date_outputs_are_not_regenerated_unless_inputs_change(self) -> None:
        self.assertEqual(self.run_script("--require-pin").returncode, 0)
        self.assertEqual(self.run_script("--require-pin").returncode, 0)
        self.assertEqual(self.calls(), 1)
        self.assertEqual(self.run_script("--require-pin", "--force").returncode, 0)
        self.assertEqual(self.calls(), 2)
        (self.root / "cmd" / "vmafx-node" / "bpf" / "rclone_bypass.bpf.c").write_text(
            "changed", encoding="utf-8"
        )
        self.assertEqual(self.run_script("--require-pin").returncode, 0)
        self.assertEqual(self.calls(), 3)

    def test_every_node_build_path_runs_the_generator(self) -> None:
        """Dropping the call from a build path would leave its Go build without the object."""
        callers = {
            ".github/actions/gen-node-bpf/action.yml": "gen-node-bpf.sh --require-pin",
            ".github/workflows/go-ci.yml": "./.github/actions/gen-node-bpf",
            "docker/Dockerfile.node": "gen-node-bpf.sh --require-pin",
            "dev/Containerfile": "gen-node-bpf.sh --require-pin",
            "Makefile": "scripts/dev/gen-node-bpf.sh $(BPF_PIN)",
        }
        for path, needle in callers.items():
            with self.subTest(path=path):
                self.assertIn(needle, (REPO / path).read_text(encoding="utf-8"))
        config = (REPO / "build-config.env").read_text(encoding="utf-8")
        major = PIN.split(".", maxsplit=1)[0]
        self.assertIn(f'BPF_CLANG_VERSION="{PIN}"', config)
        for path in (
            "docker/Dockerfile.node",
            "dev/Containerfile",
            ".github/actions/gen-node-bpf/action.yml",
        ):
            with self.subTest(clang_package=path):
                self.assertIn(f"clang-{major}", (REPO / path).read_text(encoding="utf-8"))

    @unittest.skipUnless(HAS_LIBBPF, "needs /usr/include/bpf/bpf_helpers.h (libbpf-dev)")
    def test_stale_committed_binding_fails_with_require_pin(self) -> None:
        stale = self.run_script("--require-pin", binding="package bpf // regenerated differently")
        self.assertEqual(stale.returncode, 4, stale.stderr)
        self.assertIn("rclonebypass_bpfel.go is stale", stale.stderr)

    @unittest.skipUnless(HAS_LIBBPF, "needs /usr/include/bpf/bpf_helpers.h (libbpf-dev)")
    def test_changed_binding_without_require_pin_is_reported(self) -> None:
        changed = self.run_script("--force", binding="package bpf // changed")
        self.assertEqual(changed.returncode, 0, changed.stderr)
        self.assertIn("commit the regenerated binding", changed.stderr)

    def test_repository_ignores_the_generated_files(self) -> None:
        """Assert that generated eBPF artifacts are ignored by Git."""
        for name in ("rclonebypass_bpfel.o", ".rclonebypass.stamp"):
            result = subprocess.run(  # noqa: S603 - fixed argv
                [GIT, "check-ignore", "-q", f"cmd/vmafx-node/bpf/{name}"],
                cwd=REPO,
                check=False,
                timeout=GIT_TIMEOUT_S,
            )
            self.assertEqual(result.returncode, 0, f"{name} is not git-ignored")

    def test_repository_tracks_no_ebpf_object(self) -> None:
        """Assert that no compiled eBPF object files are tracked in the repository."""
        tracked = subprocess.run(  # noqa: S603 - fixed argv
            [GIT, "ls-files", "cmd/vmafx-node/bpf"],
            cwd=REPO,
            capture_output=True,
            text=True,
            check=True,
            timeout=GIT_TIMEOUT_S,
        ).stdout.split()
        self.assertEqual([name for name in tracked if name.endswith(".o")], [])
        self.assertTrue(os.access(SCRIPT, os.X_OK))

    def test_committed_binding_embeds_no_object(self) -> None:
        """The binding is committed source: it must reach the object only through embeddedObject()."""
        binding = (REPO / "cmd" / "vmafx-node" / "bpf" / "rclonebypass_bpfel.go").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("//go:embed", binding)
        self.assertIn("var _RcloneBypassBytes = embeddedObject()", binding)

    def test_embed_rewrite_is_idempotent_and_refuses_unknown_output(self) -> None:
        """Assert that embedding rewrite is idempotent and rejects input lacking go:embed."""
        script = REPO / "cmd" / "vmafx-node" / "bpf" / "embed_generated_object.sh"
        raw = self.tmp / "raw.go"
        raw.write_text(
            'package bpf\n\nimport (\n\t_ "embed"\n)\n\n//go:embed rclonebypass_bpfel.o\nvar _RcloneBypassBytes []byte\n',
            encoding="utf-8",
        )
        for _ in range(2):
            done = subprocess.run(  # noqa: S603 - fixed argv
                ["/bin/sh", str(script), str(raw)],
                capture_output=True,
                text=True,
                check=False,
                timeout=SCRIPT_TIMEOUT_S,
            )
            self.assertEqual(done.returncode, 0, done.stderr)
        text = raw.read_text(encoding="utf-8")
        self.assertNotIn("go:embed", text)
        self.assertEqual(text.count("var _RcloneBypassBytes = embeddedObject()"), 1)
        other = self.tmp / "other.go"
        other.write_text("package bpf\n", encoding="utf-8")
        refused = subprocess.run(  # noqa: S603 - fixed argv
            ["/bin/sh", str(script), str(other)],
            capture_output=True,
            text=True,
            check=False,
            timeout=SCRIPT_TIMEOUT_S,
        )
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn("go:embed", refused.stderr)


if __name__ == "__main__":
    unittest.main()
