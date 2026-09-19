#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Execute the real envtest helper and Make consumers in a private fixture."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

# A hang detector, not a timing assertion: these subprocesses finish in tens of
# milliseconds locally, but a loaded CI runner has blown a 10-second cap and the
# TimeoutExpired then reads as a real test failure (bug ledger L-76). 120s still
# catches a genuine hang long before the job's own timeout.
SUBPROCESS_TIMEOUT_S = 120

ROOT = Path(__file__).resolve().parents[3]
MODULE = "sigs.k8s.io/controller-runtime/tools/setup-envtest"
BASH = shutil.which("bash") or "/bin/bash"
MAKE = shutil.which("make") or "/usr/bin/make"


class EnvtestSingleSource(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="envtest fixture ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.helper = self.root / "scripts/ci/setup-envtest.sh"
        self.helper.parent.mkdir(parents=True)
        shutil.copy2(ROOT / "scripts/ci/setup-envtest.sh", self.helper)
        shutil.copy2(ROOT / "Makefile", self.root / "Makefile")
        (self.root / "build-config.env").write_text(
            'SETUP_ENVTEST_VERSION="v8.9.10"\nENVTEST_K8S_VERSION="1.31"\n'
        )
        self.bin = self.root / "tool bin"
        self.bin.mkdir()
        fake = self.root / "fake"
        fake.mkdir()
        self.env = {
            k: v
            for k, v in os.environ.items()
            if not k.startswith(("GIT_", "MAKE", "GNUMAKE"))
            and k not in {"MFLAGS", "ENVTEST_K8S_VERSION", "GOBIN", "GOPATH"}
        }
        self.env.update(
            PATH=str(fake) + os.pathsep + os.defpath,
            GOBIN=str(self.bin),
            GOPATH=str(self.root / "go path"),
            FIXTURE_ROOT=str(self.root),
            ASSET_PATH=str(self.root / "assets with spaces"),
        )
        (fake / "go").write_text("#!" + sys.executable + "\n" + r"""
import json,os,pathlib,shutil,sys
root=pathlib.Path(os.environ["FIXTURE_ROOT"])
with (root/"go.jsonl").open("a") as f:f.write(json.dumps(sys.argv[1:])+"\n")
a=sys.argv[1:]
if a[0]=="env":print(os.environ.get(a[1],""))
elif a[0]=="install":
 if os.environ.get("FAIL_INSTALL"):sys.exit(17)
 target=pathlib.Path(os.environ["GOBIN"]);target.mkdir(parents=True,exist_ok=True)
 shutil.copy2(root/"tool-template",target/"setup-envtest")
 (target/"version").write_text(a[1].split("@",1)[1])
elif a[:2]==["version","-m"]:
 p=pathlib.Path(a[2]);print("mod\tsigs.k8s.io/controller-runtime/tools/setup-envtest\t"+(p.parent/"version").read_text())
else:sys.exit(19)
""")
        (fake / "go").chmod(0o755)
        (self.root / "tool-template").write_text("#!" + sys.executable + "\n" + r"""
import json,os,pathlib,sys
root=pathlib.Path(os.environ["FIXTURE_ROOT"])
with (root/"tool.jsonl").open("a") as f:f.write(json.dumps(sys.argv[1:])+"\n")
if os.environ.get("MISSING_ASSETS"):
 if "--installed-only" in sys.argv:sys.exit(24)
 (root/"download-attempt").touch()
if os.environ.get("FAIL_ASSETS"):sys.exit(23)
if not os.environ.get("EMPTY_ASSETS"):print(os.environ["ASSET_PATH"])
""")
        (self.root / "tool-template").chmod(0o755)
        # A stale PATH command must never supply assets or satisfy the pin.
        (fake / "setup-envtest").write_text("#!/bin/sh\nexit 99\n")
        (fake / "setup-envtest").chmod(0o755)

    def run_command(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 -- fixed fixture command vectors, no shell
            list(args),
            cwd=self.root,
            env=self.env,
            text=True,
            capture_output=True,
            timeout=SUBPROCESS_TIMEOUT_S,
        )

    def helper_run(self, mode: str) -> subprocess.CompletedProcess[str]:
        return self.run_command(BASH, str(self.helper), mode)

    def calls(self, filename: str) -> list[list[str]]:
        path = self.root / filename
        if not path.exists():
            return []
        return [json.loads(line) for line in path.read_text().splitlines()]

    def test_install_consumes_config_and_exact_destination(self) -> None:
        result = self.helper_run("install")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), self.env["ASSET_PATH"])
        self.assertIn(["install", MODULE + "@v8.9.10"], self.calls("go.jsonl"))
        self.assertIn(["version", "-m", str(self.bin / "setup-envtest")], self.calls("go.jsonl"))
        self.assertEqual(
            self.calls("tool.jsonl"), [["use", "1.31", "-p", "path", "--use-env=false"]]
        )

    def test_failed_install_never_uses_existing_tool(self) -> None:
        self.assertEqual(self.helper_run("install").returncode, 0)
        (self.root / "tool.jsonl").unlink()
        self.env["FAIL_INSTALL"] = "1"
        self.assertEqual(self.helper_run("install").returncode, 17)
        self.assertEqual(self.calls("tool.jsonl"), [])

    def test_stale_or_missing_tool_cannot_export(self) -> None:
        self.assertNotEqual(self.helper_run("env").returncode, 0)
        self.assertEqual(self.helper_run("install").returncode, 0)
        (self.bin / "version").write_text("v0.1.0")
        (self.root / "tool.jsonl").unlink()
        self.assertNotEqual(self.helper_run("env").returncode, 0)
        self.assertEqual(self.calls("tool.jsonl"), [])

    def test_download_failure_and_empty_path_fail(self) -> None:
        self.assertEqual(self.helper_run("install").returncode, 0)
        self.env["FAIL_ASSETS"] = "1"
        self.assertEqual(self.helper_run("path").returncode, 23)
        del self.env["FAIL_ASSETS"]
        self.env["EMPTY_ASSETS"] = "1"
        self.assertNotEqual(self.helper_run("path").returncode, 0)

    def test_missing_cache_export_never_downloads(self) -> None:
        self.assertEqual(self.helper_run("install").returncode, 0)
        self.env["MISSING_ASSETS"] = "1"
        self.env["ENVTEST_USE_ENV"] = "true"
        self.env["KUBEBUILDER_ASSETS"] = "/unrelated/assets"
        for mode in ("path", "env"):
            with self.subTest(mode=mode):
                result = self.helper_run(mode)
                self.assertEqual(result.returncode, 24)
                self.assertEqual(result.stdout, "")
                self.assertFalse((self.root / "download-attempt").exists())
        self.assertEqual(self.helper_run("install").returncode, 0)
        self.assertTrue((self.root / "download-attempt").exists())

    def test_first_gopath_destination_and_kubernetes_override(self) -> None:
        self.env["GOBIN"] = ""
        self.env["GOPATH"] = str(self.root / "first go") + ":" + str(self.root / "second go")
        self.env["ENVTEST_K8S_VERSION"] = "1.32.1"
        self.assertEqual(self.helper_run("install").returncode, 0)
        self.assertTrue((self.root / "first go/bin/setup-envtest").is_file())
        self.assertEqual(
            self.calls("tool.jsonl"), [["use", "1.32.1", "-p", "path", "--use-env=false"]]
        )

    def test_real_make_install_and_shell_quoted_export(self) -> None:
        self.env["ASSET_PATH"] = str(self.root / "a 'quoted' path; echo unexpected")
        result = self.run_command(MAKE, "-s", "setup-envtest", "ENVTEST_K8S_VERSION=1.32.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_command(MAKE, "-s", "setup-envtest-env", "ENVTEST_K8S_VERSION=1.32.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        export = self.root / "export.sh"
        export.write_text(result.stdout + 'printf "%s" "$KUBEBUILDER_ASSETS"\n')
        evaluated = self.run_command(BASH, str(export))
        self.assertEqual(evaluated.returncode, 0, evaluated.stderr)
        self.assertEqual(evaluated.stdout, self.env["ASSET_PATH"])
        self.assertEqual(
            self.calls("tool.jsonl"),
            [
                ["use", "1.32.1", "-p", "path", "--use-env=false"],
                ["use", "1.32.1", "-p", "path", "--use-env=false", "--installed-only"],
            ],
        )

    def test_floating_pin_rejected_before_install(self) -> None:
        (self.root / "build-config.env").write_text(
            'SETUP_ENVTEST_VERSION="latest"\nENVTEST_K8S_VERSION="1.31"\n'
        )
        self.assertEqual(self.helper_run("install").returncode, 2)
        self.assertEqual(self.calls("go.jsonl"), [])

    def test_ci_uses_shared_installer_and_propagates_path(self) -> None:
        workflow = (ROOT / ".github/workflows/go-ci.yml").read_text()
        self.assertIn('ASSETS="$(scripts/ci/setup-envtest.sh install)"', workflow)
        self.assertIn('echo "KUBEBUILDER_ASSETS=${ASSETS}" >> "${GITHUB_ENV}"', workflow)
        self.assertNotIn("setup-envtest@latest", workflow)
        self.assertNotIn("setup-envtest@latest", (ROOT / "Makefile").read_text())
        block = workflow.split("- name: Install envtest binaries (kubebuilder)", 1)[1]
        block = block.split("\n      - name:", 1)[0]
        script = self.root / "ci-step.sh"
        script.write_text(textwrap.dedent(block.split("        run: |\n", 1)[1]))
        output = self.root / "github-env"
        self.env["GITHUB_ENV"] = str(output)
        result = self.run_command(BASH, "-e", "-o", "pipefail", str(script))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_text(), "KUBEBUILDER_ASSETS=" + self.env["ASSET_PATH"] + "\n")
        output.unlink()
        self.env["FAIL_INSTALL"] = "1"
        result = self.run_command(BASH, "-e", "-o", "pipefail", str(script))
        self.assertEqual(result.returncode, 17)
        self.assertFalse(output.exists())

    def test_renovate_tracks_the_consumed_config_only(self) -> None:
        config = json.loads((ROOT / "renovate.json").read_text())
        managers = [m for m in config["customManagers"] if m.get("depNameTemplate") == MODULE]
        self.assertEqual(len(managers), 1)
        manager = managers[0]
        self.assertEqual(manager["datasourceTemplate"], "go")
        self.assertEqual(manager["managerFilePatterns"], ["/^build-config\\.env$/"])
        pattern = manager["matchStrings"][0].replace("(?<currentValue>", "(?P<currentValue>")
        found = re.search(pattern, (ROOT / "build-config.env").read_text())
        self.assertIsNotNone(found)
        assert found is not None
        self.assertRegex(found["currentValue"], r"^v[0-9]+\.[0-9]+\.[0-9]+$")


if __name__ == "__main__":
    unittest.main()
