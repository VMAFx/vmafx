#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Regression tests for the optional NEO GitHub build-secret contract."""

from __future__ import annotations

import importlib.util
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import cast

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

_BASH = shutil.which("bash")
BASH = str(Path(_BASH).resolve(strict=True)) if _BASH is not None else "bash"
CHECKER = ROOT / "scripts/ci/check-dev-container-build-secret.py"
SPEC = importlib.util.spec_from_file_location("check_dev_container_build_secret", CHECKER)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

GATE = ".github/workflows/dev-container-build.yml"
RELEASE = ".github/workflows/supply-chain.yml"
REHEARSAL_BUILD = "bash scripts/ci/build-dev-container-stage.sh release-build \\\n"
STEP_CREDENTIAL_ENV = "        env:\n          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}\n"
BUILDER_FORWARDING_ARM = 'secret_args=(--secret "id=github_token,env=GITHUB_TOKEN")'


class BuildSecretContract(unittest.TestCase):
    def setUp(self) -> None:
        self.containerfile = (ROOT / "dev/Containerfile").read_text(encoding="utf-8")
        self.compose = (ROOT / "dev/docker-compose.yml").read_text(encoding="utf-8")
        self.workflow = (ROOT / GATE).read_text(encoding="utf-8")
        self.docs = (ROOT / "docs/development/dev-mcp.md").read_text(encoding="utf-8")
        self.fetcher = (ROOT / "dev/scripts/fetch-intel-neo.py").read_text(encoding="utf-8")
        self.publish = (ROOT / ".github/workflows/dev-container-publish.yml").read_text()
        self.builder = (ROOT / MODULE.STAGE_BUILDER).read_text(encoding="utf-8")
        self.release = (ROOT / RELEASE).read_text(encoding="utf-8")

    def errors(
        self,
        *,
        containerfile: str | None = None,
        compose: str | None = None,
        workflow: str | None = None,
        docs: str | None = None,
        fetcher: str | None = None,
    ) -> list[str]:
        containerfile = containerfile or self.containerfile
        errors = MODULE.validate_containerfile(containerfile)
        errors.extend(MODULE.validate_compose(compose or self.compose))
        errors.extend(
            MODULE.validate_callers(
                workflow or self.workflow,
                docs or self.docs,
                fetcher or self.fetcher,
                containerfile,
            )
        )
        return cast(list[str], errors)

    def builder_errors(self, builder: str) -> list[str]:
        return cast(list[str], MODULE.validate_stage_builder(builder, self.containerfile))

    def caller_errors(self, workflow: str, path: str) -> list[str]:
        label, expected = MODULE.STAGE_CALLERS[path]
        return cast(
            list[str], MODULE.validate_stage_caller(workflow, label, expected, self.containerfile)
        )

    def test_current_tree_passes(self) -> None:
        self.assertEqual(self.errors(), [])
        self.assertEqual(MODULE.validate_tree(ROOT), [])

    def test_arg_and_env_token_declarations_fail(self) -> None:
        for declaration in ('ARG GITHUB_TOKEN=""', "ENV GITHUB_TOKEN=secret"):
            with self.subTest(declaration=declaration):
                text = self.containerfile + f"\n{declaration}\n"
                self.assertTrue(
                    any("ARG or ENV" in error for error in self.errors(containerfile=text))
                )

    def test_required_secret_fails(self) -> None:
        text = self.containerfile.replace("required=false", "required=true", 1)
        self.assertTrue(any("required=false" in error for error in self.errors(containerfile=text)))

    def test_missing_compose_source_or_build_grant_fails(self) -> None:
        for needle in ("    environment: GITHUB_TOKEN\n", "          target: github_token\n"):
            with self.subTest(needle=needle.strip()):
                text = self.compose.replace(needle, "", 1)
                self.assertTrue(any("Compose" in error for error in self.errors(compose=text)))

    # --- which stages consume the secret -----------------------------------

    def test_secret_consumers_follow_the_stage_graph(self) -> None:
        stages, consumers = MODULE.secret_stages(self.containerfile)
        self.assertLessEqual({"build-deps", "release-build", "gpu-sdks", "libvmaf-build"}, stages)
        self.assertNotIn("build-deps", consumers)
        self.assertNotIn("release-build", consumers)
        self.assertLessEqual({"gpu-sdks", "libvmaf-build", "dev-mcp"}, consumers)

    def test_copy_from_a_consumer_consumes(self) -> None:
        text = (
            "FROM scratch AS base\n"
            "FROM base AS fetch\n"
            "RUN --mount=type=secret,id=github_token,env=GITHUB_TOKEN,required=false true\n"
            "FROM base AS plain\n"
            "FROM scratch AS assembler\n"
            "COPY --from=fetch /x /x\n"
        )
        _, consumers = MODULE.secret_stages(text)
        self.assertEqual(consumers, {"fetch", "assembler"})

    # --- the stage builder script ------------------------------------------

    def test_current_stage_builder_and_release_caller_pass(self) -> None:
        self.assertEqual(self.builder_errors(self.builder), [])
        self.assertEqual(self.caller_errors(self.release, RELEASE), [])

    def test_stage_builder_without_secret_fails(self) -> None:
        self.assertIn(BUILDER_FORWARDING_ARM, self.builder)
        text = self.builder.replace(BUILDER_FORWARDING_ARM, "secret_args=()", 1)
        self.assertTrue(
            any("github_token for libvmaf-build" in error for error in self.builder_errors(text))
        )

    def test_stage_builder_secret_for_release_build_fails(self) -> None:
        text = self.builder.replace("secret_args=()", BUILDER_FORWARDING_ARM, 1)
        self.assertTrue(
            any("must not pass a secret for release-build" in e for e in self.builder_errors(text))
        )

    def test_stage_builder_secret_for_every_target_fails(self) -> None:
        text = self.builder.replace(
            '  "${secret_args[@]}" \\\n',
            f'  "${{secret_args[@]}}" \\\n  {MODULE.BUILD_OPTION} \\\n',
            1,
        )
        self.assertTrue(
            any("not pass a secret for every target" in e for e in self.builder_errors(text))
        )

    def test_stage_builder_unknown_or_missing_targets_fail(self) -> None:
        unknown = self.builder.replace("  release-build)\n", "  release-image)\n", 1)
        self.assertTrue(
            any("release-image, not a dev/Containerfile" in e for e in self.builder_errors(unknown))
        )
        no_case = self.builder.replace("  libvmaf-build)\n", "", 1).replace(
            "  release-build)\n", "", 1
        )
        self.assertTrue(any("allowlist" in e for e in self.builder_errors(no_case)))

    def test_stage_builder_build_arg_or_cache_fails(self) -> None:
        for extra, expected in (
            ("  --build-arg FOO=bar \\\n", "build arguments"),
            ("  --cache-from type=gha \\\n", "cache-free"),
            ("  --cache-to type=gha,mode=max \\\n", "cache-free"),
        ):
            with self.subTest(extra=extra.strip()):
                text = self.builder.replace(
                    '  --target "$target"', extra + '  --target "$target"', 1
                )
                self.assertTrue(any(expected in error for error in self.builder_errors(text)))

    # --- workflow callers ----------------------------------------------------

    def test_missing_raw_build_wiring_fails(self) -> None:
        for needle in (MODULE.STAGE_BUILDER, MODULE.CALLER_ENV_LINE):
            with self.subTest(needle=needle):
                text = self.workflow.replace(needle, "", 1)
                self.assertTrue(
                    any("raw CI build" in error for error in self.errors(workflow=text))
                )

    def test_gate_without_release_rehearsal_fails(self) -> None:
        self.assertIn(REHEARSAL_BUILD, self.workflow)
        text = self.workflow.replace(REHEARSAL_BUILD, "true \\\n", 1)
        self.assertTrue(
            any("build libvmaf-build, release-build" in e for e in self.caller_errors(text, GATE))
        )

    def test_release_on_the_dev_track_stage_fails(self) -> None:
        # ADR-1354: the release compiles in the Debian 13 release-track stage.
        # Pointing the release job back at build-deps (Ubuntu 26.04) must fail.
        text = self.release.replace(
            "build-dev-container-stage.sh release-build \\\n",
            "build-dev-container-stage.sh build-deps \\\n",
            1,
        )
        self.assertNotEqual(text, self.release)
        self.assertTrue(
            any("must build release-build" in e for e in self.caller_errors(text, RELEASE))
        )

    def test_token_on_a_release_build_step_fails(self) -> None:
        for path, workflow in ((GATE, self.workflow), (RELEASE, self.release)):
            with self.subTest(path=path):
                step = (
                    "Build the release-build"
                    if path == RELEASE
                    else "build the release-build stage"
                )
                start = workflow.index(step)
                run_at = workflow.index("        run: |\n", start)
                text = workflow[:run_at] + STEP_CREDENTIAL_ENV + workflow[run_at:]
                self.assertTrue(
                    any(
                        "of release-build must not receive GITHUB_TOKEN" in e
                        for e in self.caller_errors(text, path)
                    )
                )

    def test_job_level_token_fails(self) -> None:
        text = self.release.replace(
            "    runs-on: ubuntu-latest\n    timeout-minutes: 60\n",
            "    runs-on: ubuntu-latest\n    timeout-minutes: 60\n"
            "    env:\n      GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}\n",
            1,
        )
        self.assertNotEqual(text, self.release)
        self.assertTrue(any("for a whole job" in e for e in self.caller_errors(text, RELEASE)))

    def test_release_caller_without_script_fails(self) -> None:
        text = self.release.replace(MODULE.STAGE_BUILDER, "", 1)
        self.assertTrue(
            any("release build must build" in e for e in self.caller_errors(text, RELEASE))
        )

    def test_inline_stage_build_fails_but_check_passes(self) -> None:
        inline = self.release.replace(
            "bash scripts/ci/build-dev-container-stage.sh release-build \\\n",
            "docker build --file dev/Containerfile --target release-build \\\n",
            1,
        )
        self.assertNotEqual(inline, self.release)
        self.assertTrue(any("only via" in e for e in self.caller_errors(inline, RELEASE)))
        self.assertIn("docker build \\\n            --check", self.workflow)
        self.assertEqual(self.caller_errors(self.workflow, GATE), [])

    # --- the publisher and the docs ------------------------------------------

    def test_release_rehearsal_matches_the_release_run(self) -> None:
        self.assertEqual(MODULE.validate_release_rehearsal(self.release, self.workflow), [])

    def test_release_rehearsal_drift_fails(self) -> None:
        drifted = self.workflow.replace("--workdir /src \\", "--workdir /src --privileged \\", 1)
        self.assertNotEqual(drifted, self.workflow)
        errors = MODULE.validate_release_rehearsal(self.release, drifted)
        self.assertTrue(any("same docker run" in e for e in errors), errors)

    def test_release_run_without_isolation_flags_fails(self) -> None:
        for flag in ("--pull never", "--network none"):
            with self.subTest(flag=flag):
                text = self.release.replace(f"{flag} \\\n", "", 1)
                self.assertNotEqual(text, self.release)
                errors = MODULE.validate_release_rehearsal(text, self.workflow)
                self.assertTrue(any(f"lacks {flag}" in e for e in errors), errors)

    def test_release_run_with_another_image_fails(self) -> None:
        # The image name also appears in the stage-build step; replace the
        # docker run's own image line.
        line = '            "vmafx-release-build:${GITHUB_SHA}" \\\n'
        self.assertEqual(self.release.count(line), 1)
        text = self.release.replace(line, "            ghcr.io/vmafx/vmafx-dev-mcp:master \\\n")
        self.assertNotEqual(text, self.release)
        errors = MODULE.validate_release_rehearsal(text, self.workflow)
        self.assertTrue(any("built in the same job" in e for e in errors), errors)

    def test_missing_rehearsal_run_fails(self) -> None:
        text = self.workflow.replace(
            "scripts/release/build-native-release-artifacts.sh", "scripts/release/other.sh"
        )
        errors = MODULE.validate_release_rehearsal(self.release, text)
        self.assertTrue(any("exactly one docker run (found 0)" in e for e in errors), errors)

    def test_current_publish_workflow_passes(self) -> None:
        self.assertEqual(MODULE.validate_publish(self.publish), [])

    def test_publish_without_build_secret_fails(self) -> None:
        text = self.publish.replace("github_token=${{ secrets.GITHUB_TOKEN }}", "", 1)
        self.assertTrue(any("publish build" in error for error in MODULE.validate_publish(text)))

    def test_publish_token_build_arg_fails(self) -> None:
        text = self.publish + (
            "          build-args: |\n            GITHUB_TOKEN=${{ secrets.GITHUB_TOKEN }}\n"
        )
        self.assertTrue(any("build arguments" in error for error in MODULE.validate_publish(text)))

    def test_missing_anonymous_build_documentation_fails(self) -> None:
        text = self.docs.replace("env -u GITHUB_TOKEN docker build", "docker build", 1)
        self.assertTrue(any("anonymous raw-build" in error for error in self.errors(docs=text)))


class StageBuilderBehaviour(unittest.TestCase):
    """Run the real script against a stub `docker` that records its arguments."""

    def run_builder(self, *args: str, credential: str | None = "dummy") -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as scratch:
            stub = Path(scratch) / "docker"
            log = Path(scratch) / "docker.log"
            stub.write_text(f'#!/bin/sh\nprintf "%s\\n" "$@" > "{log}"\n', encoding="utf-8")
            stub.chmod(0o755)
            env = {key: value for key, value in os.environ.items() if key != "GITHUB_TOKEN"}
            env["PATH"] = f"{scratch}{os.pathsep}{env.get('PATH', '')}"
            if credential is not None:
                env["GITHUB_TOKEN"] = credential
            result = run_command(
                [BASH, str(ROOT / MODULE.STAGE_BUILDER), *args],
                allowed_executables=(BASH,),
                env=env,
                capture_output=True,
                text=True,
            )
            recorded = log.read_text(encoding="utf-8") if log.exists() else ""
        return result.returncode, recorded

    def test_release_build_gets_no_secret_even_with_a_token(self) -> None:
        status, recorded = self.run_builder("release-build", "img:1")
        self.assertEqual(status, 0)
        lines = recorded.splitlines()
        self.assertEqual(lines[:2], ["build", "--file"])
        self.assertIn("--target\nrelease-build\n--tag\nimg:1\n", recorded)
        self.assertNotIn("--secret", recorded)

    def test_libvmaf_build_gets_the_optional_secret(self) -> None:
        for credential in ("dummy", None):
            with self.subTest(credential=credential):
                status, recorded = self.run_builder("libvmaf-build", "img:2", credential=credential)
                self.assertEqual(status, 0)
                self.assertIn("--target\nlibvmaf-build\n", recorded)
                self.assertIn("--secret\nid=github_token,env=GITHUB_TOKEN\n", recorded)

    def test_bad_invocations_exit_2_without_building(self) -> None:
        for args in (
            ("img:3",),
            ("dev-mcp", "img:3"),
            # ADR-1354: no caller builds the dev-track stage any more.
            ("build-deps", "img:3"),
            ("", "img:3"),
            ("release-build", ""),
            ("release-build", "img:3", "extra"),
        ):
            with self.subTest(args=args):
                status, recorded = self.run_builder(*args)
                self.assertEqual(status, 2)
                self.assertEqual(recorded, "")


if __name__ == "__main__":
    unittest.main()
