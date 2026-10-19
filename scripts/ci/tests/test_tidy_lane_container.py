#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The clang-tidy lanes are measured in the dev container (ADR-1471).

Three things can drift apart without anyone seeing it, and each one turns a
baseline into numbers no other machine reproduces:

* what a lane configures (``TIDY_RATCHET_SETUP_<lane>`` in the Makefile) and
  what the hosted ``Tidy Ratchet`` job configures for ``cpu``;
* the clang-tidy the container entry point installs and the one the hosted job
  installs;
* what ``scripts/dev/tidy-lane.sh`` sends into the container and brings back.

The host side of the entry point is driven here against a stand-in ``docker``,
so the cases run without the image.
"""

from __future__ import annotations

import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MAKEFILE = ROOT / "Makefile"
SCRIPT = ROOT / "scripts/dev/tidy-lane.sh"
HIP_WRAPPER = ROOT / "scripts/ci/clang-tidy-hip.sh"
WORKFLOW = ROOT / ".github/workflows/lint-and-format.yml"
NIGHTLY = ROOT / ".github/workflows/nightly.yml"
CONTAINERFILE = ROOT / "dev/Containerfile"
CONTAINER_LANES = ("cpu", "clang", "cuda", "hip", "sycl", "arm64")

# The entry point is a bash script that drives docker and GNU tar: it runs on
# the Linux workstation that has the dev image. Elsewhere its cases are skipped
# and the configuration contract above still runs.
LINUX_ONLY = unittest.skipUnless(
    sys.platform.startswith("linux"),
    "scripts/dev/tidy-lane.sh needs bash, docker and GNU tar: the lanes run from a Linux host",
)
POSIX_ONLY = unittest.skipUnless(
    os.name == "posix", "the clang-tidy wrappers are POSIX shell scripts"
)

FAKE_DOCKER = """#!/bin/sh
# Stand-in for docker: records every call, keeps the tar stream of `cp -`.
printf '%s\\n' "$*" >>"$FAKE_DOCKER_DIR/calls"
case "$1" in
  image) exit "${FAKE_IMAGE_RC:-0}" ;;
  create) echo fakecid ;;
  cp)
    if [ "$2" = "-" ]; then
      cat >"$FAKE_DOCKER_DIR/source.tar"
    else
      cp "$FAKE_DOCKER_DIR"/out/* "$3" 2>/dev/null || exit 1
    fi
    ;;
  start) exit "${FAKE_START_RC:-0}" ;;
esac
exit 0
"""


def variable(name: str) -> list[str]:
    """The words of a Makefile variable, line continuations joined."""
    text = MAKEFILE.read_text(encoding="utf-8").replace("\\\n", " ")
    match = re.search(rf"^{re.escape(name)}\s*:?=(.*)$", text, re.MULTILINE)
    if match is None:
        raise AssertionError(f"{name} is not defined in the Makefile")
    return match.group(1).split()


def job_text(workflow: Path, job_id: str) -> str:
    """One job of a workflow file, line continuations joined."""
    text = workflow.read_text(encoding="utf-8")
    start = text.index(f"\n  {job_id}:\n")
    following = re.search(r"\n  [A-Za-z0-9_-]+:\n", text[start + 1 :])
    end = start + 1 + following.start() if following else len(text)
    return text[start:end].replace("\\\n", " ")


# The hosted jobs that measure the cpu lane: the required `Tidy Ratchet` and the
# nightly full scan.
HOSTED_CPU_JOBS = ((WORKFLOW, "clang-tidy-ratchet"), (NIGHTLY, "clang-tidy-full"))


class LaneConfiguration(unittest.TestCase):
    def test_hosted_cpu_jobs_run_the_makefile_lane(self) -> None:
        """T-TIDY-RATCHET-UNMEASURED-AS-CLEAN-2026-10-06: one definition of the cpu lane.

        A hosted job that repeated the lane's `meson setup` line still built
        another set of translation units: it left out the MATLAB MEX compile
        commands (TIDY_RATCHET_COMPDB_cpu) the container adds.
        """
        for workflow, job_id in HOSTED_CPU_JOBS:
            with self.subTest(job=job_id):
                job = job_text(workflow, job_id)
                self.assertRegex(job, r"make tidy-ratchet-build LANE=cpu ")
                self.assertRegex(job, r"make tidy-ratchet LANE=cpu ")
                self.assertNotIn("meson setup", job)
                self.assertNotIn("scripts/ci/tidy-ratchet.py --lane", job)
                self.assertNotIn("write-compile-commands.py", job)

    def test_hosted_cpu_jobs_install_the_containers_vpl(self) -> None:
        """The container has libvpl-dev, so its cpu build has the VPL translation units."""
        self.assertIn("libvpl-dev", CONTAINERFILE.read_text(encoding="utf-8"))
        for workflow, job_id in HOSTED_CPU_JOBS:
            with self.subTest(job=job_id):
                self.assertRegex(
                    job_text(workflow, job_id), r"apt-get install -y [^\n]*\blibvpl-dev\b"
                )

    def test_cpu_lane_adds_the_mex_compile_commands(self) -> None:
        self.assertIn("scripts/ci/gen-mex-compile-commands.py", variable("TIDY_RATCHET_COMPDB_cpu"))

    def test_cpu_measures_no_optional_runtime(self) -> None:
        """The hosted runner has no ONNX Runtime; the container does."""
        self.assertIn("-Denable_dnn=disabled", variable("TIDY_RATCHET_SETUP_cpu"))

    def test_gpu_lanes_turn_their_device_compiler_on(self) -> None:
        required = {
            "cuda": {"-Denable_cuda=true", "-Denable_nvcc=true"},
            "hip": {"-Denable_hip=true", "-Denable_hipcc=true"},
            "sycl": {"-Denable_sycl=true"},
        }
        for lane, flags in required.items():
            with self.subTest(lane=lane):
                options = set(variable(f"TIDY_RATCHET_SETUP_{lane}"))
                self.assertLessEqual(flags, options)
                self.assertIn("-Denable_dnn=enabled", options)

    def test_every_lane_configures_without_lto(self) -> None:
        for lane in CONTAINER_LANES:
            with self.subTest(lane=lane):
                self.assertIn("-Db_lto=false", variable(f"TIDY_RATCHET_SETUP_{lane}"))

    def test_arm64_cross_files_exist_and_name_the_containers_emulator(self) -> None:
        """Ubuntu 26.04 has `qemu-aarch64`; the first cross file names the static one."""
        setup = variable("TIDY_RATCHET_SETUP_arm64")
        files = [setup[i + 1] for i, word in enumerate(setup) if word == "--cross-file"]
        self.assertEqual(
            files,
            ["build-aux/aarch64-linux-gnu.ini", "build-aux/aarch64-linux-gnu-qemu-user.ini"],
        )
        override = (ROOT / files[1]).read_text(encoding="utf-8")
        self.assertIn("exe_wrapper = ['qemu-aarch64', '-L', '/usr/aarch64-linux-gnu']", override)
        self.assertIn("qemu-user", SCRIPT.read_text(encoding="utf-8"))

    def test_cpu_reads_the_embedded_mcp_server(self) -> None:
        """core/src/mcp and its tests are in no other lane's compile database."""
        options = variable("TIDY_RATCHET_SETUP_cpu")
        for flag in (
            "-Denable_mcp=true",
            "-Denable_mcp_sse=enabled",
            "-Denable_mcp_uds=true",
            "-Denable_mcp_stdio=true",
        ):
            self.assertIn(flag, options)

    def test_clang_lane_builds_the_fuzz_harnesses_and_measures_only_them(self) -> None:
        """libFuzzer needs clang; the gcc lanes cannot configure -Dfuzz=true."""
        self.assertEqual(
            variable("TIDY_RATCHET_COMPILERS_clang"), ["CC=clang-22", "CXX=clang++-22"]
        )
        self.assertIn("-Dfuzz=true", variable("TIDY_RATCHET_SETUP_clang"))
        self.assertEqual(
            variable("TIDY_RATCHET_EXTRA_clang"),
            ["--select", "core/test/fuzz/", "--select", "core/src/read_json_model.c"],
        )

    def test_metal_job_is_the_makefile_configuration(self) -> None:
        """The macOS lane has no container; its job repeats the Makefile line."""
        job = (ROOT / ".github/workflows/tidy-metal.yml").read_text(encoding="utf-8")
        job = job.replace("\\\n", " ")
        match = re.search(
            r'^\s*((?:[A-Z]+=\S+\s+)*)meson setup "\$TIDY_BUILD_DIR" core(.*)$', job, re.MULTILINE
        )
        self.assertIsNotNone(match)
        assert match is not None
        self.assertEqual(match.group(1).split(), variable("TIDY_RATCHET_COMPILERS_metal"))
        self.assertEqual(
            sorted(match.group(2).split()), sorted(variable("TIDY_RATCHET_SETUP_metal"))
        )
        self.assertIn("llvm@22", job)
        self.assertTrue((ROOT / "scripts/ci/tidy-baseline-metal.json").is_file())

    def test_hip_lane_dispatches_kernels_to_the_rocm_clang_tidy(self) -> None:
        self.assertIn("$(CURDIR)/scripts/ci/clang-tidy-hip.sh", variable("TIDY_RATCHET_EXTRA_hip"))

    def test_container_and_hosted_job_install_the_same_clang_tidy(self) -> None:
        script = SCRIPT.read_text(encoding="utf-8")
        match = re.search(r"^CLANG_TIDY_MAJOR=(\d+)$", script, re.MULTILINE)
        self.assertIsNotNone(match)
        assert match is not None
        for workflow, job_id in HOSTED_CPU_JOBS:
            with self.subTest(job=job_id):
                job = job_text(workflow, job_id)
                self.assertIn(f"sudo /tmp/llvm.sh {match.group(1)}", job)
                self.assertIn(f"CLANG_TIDY_BIN=/usr/bin/clang-tidy-{match.group(1)}", job)

    def test_make_targets_run_the_entry_point(self) -> None:
        text = MAKEFILE.read_text(encoding="utf-8")
        self.assertIn("tidy-lane:\n\tscripts/dev/tidy-lane.sh $(TIDY_LANE_ARGS) $(LANE)\n", text)
        self.assertIn(
            "tidy-lane-write:\n\tscripts/dev/tidy-lane.sh --write $(TIDY_LANE_ARGS) $(LANE)\n",
            text,
        )

    def test_a_baseline_exists_for_every_container_lane(self) -> None:
        for lane in CONTAINER_LANES:
            with self.subTest(lane=lane):
                self.assertTrue((ROOT / f"scripts/ci/tidy-baseline-{lane}.json").is_file())


@LINUX_ONLY
class HostSide(unittest.TestCase):
    """scripts/dev/tidy-lane.sh up to and after the container run."""

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.repo = self.tmp / "repo"
        (self.repo / "scripts/dev").mkdir(parents=True)
        (self.repo / "scripts/ci").mkdir()
        shutil.copy(SCRIPT, self.repo / "scripts/dev/tidy-lane.sh")
        (self.repo / "Makefile").write_text("all:\n", encoding="utf-8")
        (self.repo / "scripts/ci/tidy-baseline-cpu.json").write_text("old\n", encoding="utf-8")
        (self.repo / ".gitignore").write_text("build/\n", encoding="utf-8")
        self._git("init", "--quiet")
        self._git("add", ".")
        self._git(
            "-c",
            "user.name=t",
            "-c",
            "user.email=t@example.invalid",
            "commit",
            "--quiet",
            "-m",
            "x",
        )
        self.docker_dir = self.tmp / "docker"
        (self.docker_dir / "bin").mkdir(parents=True)
        (self.docker_dir / "out").mkdir()
        fake = self.docker_dir / "bin/docker"
        fake.write_text(FAKE_DOCKER, encoding="utf-8")
        fake.chmod(0o755)
        self.out = self.tmp / "reports"

    def _git(self, *args: str) -> None:
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        git = shutil.which("git")
        self.assertIsNotNone(git, "git is needed to build the fixture checkout")
        assert git is not None
        subprocess.run(  # noqa: S603 -- fixed Git commands in a disposable fixture
            [git, "-C", str(self.repo), *args], check=True, env=env, timeout=60
        )

    def _run(self, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
        environ = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        environ.update(
            PATH=f"{self.docker_dir / 'bin'}{os.pathsep}{environ['PATH']}",
            FAKE_DOCKER_DIR=str(self.docker_dir),
            **env,
        )
        return subprocess.run(  # noqa: S603 -- the script under test, in its fixture
            [str(self.repo / "scripts/dev/tidy-lane.sh"), "--out", str(self.out), *args],
            capture_output=True,
            text=True,
            env=environ,
            check=False,
            timeout=120,
        )

    def _calls(self) -> list[str]:
        calls = self.docker_dir / "calls"
        return calls.read_text(encoding="utf-8").splitlines() if calls.exists() else []

    def _sent(self) -> list[str]:
        data = (self.docker_dir / "source.tar").read_bytes()
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            return sorted(archive.getnames())

    def test_checkout_goes_in_as_a_tar_and_nothing_is_mounted(self) -> None:
        (self.repo / "untracked.c").write_text("int a;\n", encoding="utf-8")
        (self.repo / "build").mkdir()
        (self.repo / "build/ignored.o").write_text("x", encoding="utf-8")
        result = self._run("--jobs", "3", "cpu")
        self.assertEqual(result.returncode, 0, result.stderr)
        create = next(call for call in self._calls() if call.startswith("create "))
        self.assertIn("--cpus 3", create)
        self.assertIn("--user 0:0", create)
        self.assertNotIn(" -v ", create)
        self.assertNotIn("--mount", create)
        self.assertTrue(
            create.endswith(
                "vmaf-dev-mcp:local /tmp/vmafx-tidy/src/scripts/dev/tidy-lane.sh "
                "--in-container --jobs 3 cpu"
            ),
            create,
        )
        sent = self._sent()
        self.assertIn("vmafx-tidy/src/Makefile", sent)
        self.assertIn("vmafx-tidy/src/scripts/dev/tidy-lane.sh", sent)
        self.assertIn("vmafx-tidy/src/untracked.c", sent)  # not yet added, not ignored
        self.assertFalse([name for name in sent if "ignored.o" in name or ".git/" in name])
        self.assertEqual(self._calls()[-1], "rm -f fakecid")

    def test_all_runs_the_six_container_lanes(self) -> None:
        self.assertEqual(self._run("all").returncode, 0)
        create = next(call for call in self._calls() if call.startswith("create "))
        self.assertTrue(
            create.endswith("--in-container --jobs 8 cpu clang cuda hip sycl arm64"), create
        )

    def test_ratchet_exit_code_is_the_scripts(self) -> None:
        result = self._run("cuda", FAKE_START_RC="3")
        self.assertEqual(result.returncode, 3)
        self.assertEqual(self._calls()[-1], "rm -f fakecid")

    def test_write_brings_the_baseline_back(self) -> None:
        (self.docker_dir / "out/tidy-baseline-cpu.json").write_text("new\n", encoding="utf-8")
        (self.docker_dir / "out/tidy-ratchet-cpu.json").write_text("{}\n", encoding="utf-8")
        result = self._run("--write", "cpu")
        self.assertEqual(result.returncode, 0, result.stderr)
        baseline = self.repo / "scripts/ci/tidy-baseline-cpu.json"
        self.assertEqual(baseline.read_text(encoding="utf-8"), "new\n")
        self.assertTrue((self.out / "tidy-ratchet-cpu.json").is_file())
        create = next(call for call in self._calls() if call.startswith("create "))
        self.assertTrue(create.endswith("--in-container --jobs 8 --write cpu"), create)

    def test_write_never_copies_a_stale_baseline_from_the_report_directory(self) -> None:
        """A baseline left in the shared report directory by an earlier run is not this run's."""
        self.out.mkdir()
        (self.out / "tidy-baseline-cpu.json").write_text("stale\n", encoding="utf-8")
        (self.docker_dir / "out/tidy-ratchet-cpu.json").write_text("{}\n", encoding="utf-8")
        result = self._run("--write", "cpu")
        baseline = self.repo / "scripts/ci/tidy-baseline-cpu.json"
        self.assertEqual(baseline.read_text(encoding="utf-8"), "old\n")
        self.assertNotEqual(result.returncode, 0, "a write that produced no baseline must fail")
        self.assertIn("produced no baseline for cpu", result.stderr)

    def test_a_fresh_baseline_wins_over_a_stale_one(self) -> None:
        self.out.mkdir()
        (self.out / "tidy-baseline-cpu.json").write_text("stale\n", encoding="utf-8")
        (self.docker_dir / "out/tidy-baseline-cpu.json").write_text("new\n", encoding="utf-8")
        result = self._run("--write", "cpu")
        self.assertEqual(result.returncode, 0, result.stderr)
        baseline = self.repo / "scripts/ci/tidy-baseline-cpu.json"
        self.assertEqual(baseline.read_text(encoding="utf-8"), "new\n")

    def test_only_reaches_the_ratchet_for_a_scoped_measurement(self) -> None:
        (self.repo / "core/src").mkdir(parents=True)
        (self.repo / "core/src/a.c").write_text("int a;\n", encoding="utf-8")
        (self.docker_dir / "out/tidy-baseline-hip.json").write_text("new\n", encoding="utf-8")
        result = self._run("--write", "--only", "core/src/a.c", "--only", "Makefile", "hip")
        self.assertEqual(result.returncode, 0, result.stderr)
        create = next(call for call in self._calls() if call.startswith("create "))
        self.assertTrue(
            create.endswith("--jobs 8 --write --only core/src/a.c --only Makefile hip"), create
        )

    def test_only_rejects_paths_outside_the_checkout(self) -> None:
        for path in ("/etc/passwd", "../x.c", "missing.c", "a b.c"):
            with self.subTest(path=path):
                self.assertEqual(self._run("--only", path, "cpu").returncode, 5)
        self.assertEqual(self._calls(), [])

    def test_a_check_never_touches_the_baseline(self) -> None:
        (self.docker_dir / "out/tidy-baseline-cpu.json").write_text("new\n", encoding="utf-8")
        self.assertEqual(self._run("cpu").returncode, 0)
        baseline = self.repo / "scripts/ci/tidy-baseline-cpu.json"
        self.assertEqual(baseline.read_text(encoding="utf-8"), "old\n")

    def test_a_tracked_file_deleted_in_the_working_tree_is_skipped(self) -> None:
        (self.repo / "Makefile").unlink()
        result = self._run("cpu")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("vmafx-tidy/src/Makefile", self._sent())

    def test_unknown_lane_and_bad_arguments_are_usage_errors(self) -> None:
        for args in (("metal",), (), ("--jobs", "0", "cpu"), ("--jobs", "x", "cpu"), ("--bogus",)):
            with self.subTest(args=args):
                result = self._run(*args)
                self.assertEqual(result.returncode, 5, result.stderr)
        self.assertEqual(self._calls(), [])  # docker was never reached

    def test_missing_image_is_reported_before_anything_is_created(self) -> None:
        result = self._run("cpu", FAKE_IMAGE_RC="1")
        self.assertEqual(result.returncode, 1)
        self.assertIn("image 'vmaf-dev-mcp:local' not found", result.stderr)
        self.assertFalse([call for call in self._calls() if call.startswith("create")])


FAKE_MAKE = """#!/bin/sh
# Stand-in for make: records its arguments, fails the way make reports a
# failing recipe (exit 2 and an "Error N" line) when the test asks for it.
printf '%s\\n' "$*" >>"$FAKE_MAKE_CALLS"
lane=""
for arg in "$@"; do
  case "$arg" in LANE=*) lane="${arg#LANE=}" ;; esac
done
case "$1" in
  tidy-ratchet-build) code="${FAKE_BUILD_RC:-0}" ;;
  *) eval "code=\\${FAKE_RATCHET_RC_$lane:-0}" ;;
esac
[ "$code" -eq 0 ] && exit 0
printf 'make: *** [Makefile:1: %s] Error %s\\n' "$1" "$code"
exit 2
"""


@LINUX_ONLY
class ContainerSide(unittest.TestCase):
    """scripts/dev/tidy-lane.sh --in-container against a stand-in make and clang-tidy."""

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.work = self.tmp / "work"
        baselines = self.work / "src/scripts/ci"
        baselines.mkdir(parents=True)
        for lane in ("cpu", "cuda"):
            (baselines / f"tidy-baseline-{lane}.json").write_text(
                '{\n  "clang_tidy_version": "22.1.8",\n  "tus": 1\n}\n', encoding="utf-8"
            )
        (self.tmp / "bin").mkdir()
        make = self.tmp / "bin/make"
        make.write_text(FAKE_MAKE, encoding="utf-8")
        make.chmod(0o755)
        self.calls = self.tmp / "make-calls"

    def _run(
        self, *args: str, tidy: str = "22.1.8", **env: str
    ) -> subprocess.CompletedProcess[str]:
        clang_tidy = self.tmp / "clang-tidy"
        clang_tidy.write_text(f'#!/bin/sh\necho "Ubuntu LLVM version {tidy}"\n', encoding="utf-8")
        clang_tidy.chmod(0o755)
        environ = dict(os.environ)
        environ.update(
            PATH=f"{self.tmp / 'bin'}{os.pathsep}{environ['PATH']}",
            TIDY_LANE_WORK=str(self.work),
            TIDY_LANE_CLANG_TIDY=str(clang_tidy),
            FAKE_MAKE_CALLS=str(self.calls),
            **env,
        )
        return subprocess.run(  # noqa: S603 -- the script under test, fixed path
            [str(SCRIPT), "--in-container", "--jobs", "2", *args],
            capture_output=True,
            text=True,
            env=environ,
            check=False,
            timeout=120,
        )

    def _calls(self) -> list[str]:
        return self.calls.read_text(encoding="utf-8").splitlines() if self.calls.exists() else []

    def test_a_lane_is_built_then_measured_with_the_lane_clang_tidy(self) -> None:
        result = self._run("cpu")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        build, ratchet = self._calls()
        self.assertTrue(build.startswith("tidy-ratchet-build LANE=cpu "), build)
        self.assertIn(f"TIDY_RATCHET_BUILD_DIR={self.work}/build-cpu", build)
        self.assertIn("TIDY_RATCHET_JOBS=2", build)
        self.assertTrue(ratchet.startswith("tidy-ratchet LANE=cpu "), ratchet)
        self.assertIn(f"CLANG_TIDY_BIN={self.tmp}/clang-tidy", ratchet)
        self.assertIn(f"--jobs 2 --report {self.work}/out/tidy-ratchet-cpu.json", ratchet)
        self.assertIn("== tidy lane cpu (clang-tidy 22.1.8, 2 jobs)", result.stdout)

    def test_the_ratchets_code_is_read_back_through_make(self) -> None:
        result = self._run("cpu", FAKE_RATCHET_RC_cpu="3")
        self.assertEqual(result.returncode, 3, result.stdout)
        self.assertIn("== tidy lane cpu: exit 3", result.stdout)

    def test_the_highest_code_of_several_lanes_wins(self) -> None:
        result = self._run("cpu", "cuda", FAKE_RATCHET_RC_cpu="3", FAKE_RATCHET_RC_cuda="2")
        self.assertEqual(result.returncode, 3, result.stdout)
        self.assertIn("== tidy lane cuda: exit 2", result.stdout)

    def test_a_lane_that_does_not_build_is_not_measured(self) -> None:
        result = self._run("cpu", FAKE_BUILD_RC="1")
        self.assertEqual(result.returncode, 4, result.stdout)
        self.assertEqual(len(self._calls()), 1)  # the build; no ratchet
        self.assertIn("lane cpu did not build; nothing was measured", result.stdout)

    def test_a_check_under_another_clang_tidy_is_refused(self) -> None:
        result = self._run("cpu", tidy="22.1.9")
        self.assertEqual(result.returncode, 5, result.stdout)
        self.assertEqual(self._calls(), [])
        self.assertIn("clang-tidy 22.1.9 is installed, but", result.stdout)
        self.assertIn("tidy-baseline-cpu.json was measured with 22.1.8", result.stdout)

    def test_a_scoped_write_under_another_clang_tidy_is_refused(self) -> None:
        result = self._run("--write", "--only", "Makefile", "cpu", tidy="22.1.9")
        self.assertEqual(result.returncode, 5, result.stdout)
        self.assertEqual(self._calls(), [])

    def test_a_full_write_moves_the_baseline_to_the_installed_version(self) -> None:
        result = self._run("--write", "cpu", tidy="22.1.9")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("clang-tidy 22.1.9 replaces 22.1.8", result.stdout)
        self.assertTrue(self._calls()[1].startswith("tidy-ratchet-write LANE=cpu "))
        self.assertTrue((self.work / "out/tidy-baseline-cpu.json").is_file())

    def test_a_failed_write_leaves_no_baseline_to_copy_back(self) -> None:
        result = self._run("--write", "cpu", FAKE_RATCHET_RC_cpu="4")
        self.assertEqual(result.returncode, 4, result.stdout)
        self.assertFalse((self.work / "out/tidy-baseline-cpu.json").exists())


@POSIX_ONLY
class HipWrapper(unittest.TestCase):
    """scripts/ci/clang-tidy-hip.sh picks the clang-tidy by translation unit."""

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        for name in ("host-tidy", "rocm-tidy"):
            tool = self.tmp / name
            tool.write_text(f'#!/bin/sh\necho "{name} $*"\n', encoding="utf-8")
            tool.chmod(0o755)

    def _run(self, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
        environ = dict(os.environ)
        environ.update(
            CLANG_TIDY_BIN=str(self.tmp / "host-tidy"),
            HIP_CLANG_TIDY_BIN=str(self.tmp / "rocm-tidy"),
        )
        environ.update(env)
        return subprocess.run(  # noqa: S603 -- the wrapper under test, fixed path
            [str(HIP_WRAPPER), *args],
            capture_output=True,
            text=True,
            env=environ,
            check=False,
            timeout=60,
        )

    def test_host_translation_units_use_the_lane_clang_tidy(self) -> None:
        result = self._run(
            "-p", "build", "--extra-arg=-I/opt/rocm/include", "core/src/hip/common.c"
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(
            result.stdout.strip(),
            "host-tidy -p build --extra-arg=-I/opt/rocm/include core/src/hip/common.c",
        )

    def test_kernels_use_the_rocm_clang_tidy_on_the_host_job(self) -> None:
        # A .hip TU compiles twice, for the host and for the device, and
        # clang-tidy analyses the driver's first job. ROCm 10.0's clang (LLVM
        # 23) listed the host job first, ROCm 10.1's (LLVM 24) lists the device
        # job first: the lane measured another compilation without a word, 23
        # findings more in four headers. The wrapper names the job the
        # baselines were measured on.
        result = self._run("-p", "build", "core/src/feature/hip/integer_psnr/psnr_score.hip")
        self.assertEqual(
            result.stdout.strip(),
            "rocm-tidy --extra-arg=--cuda-host-only -p build "
            "core/src/feature/hip/integer_psnr/psnr_score.hip",
        )

    def test_host_translation_units_get_no_offload_flag(self) -> None:
        result = self._run("-p", "build", "core/src/hip/common.c")
        self.assertNotIn("--cuda-host-only", result.stdout)

    def test_version_is_the_lane_clang_tidys(self) -> None:
        self.assertEqual(self._run("--version").stdout.strip(), "host-tidy --version")

    def test_an_option_ending_in_hip_is_not_a_kernel(self) -> None:
        result = self._run("--extra-arg=-I/x.hip", "core/src/hip/common.c")
        self.assertTrue(result.stdout.startswith("host-tidy "), result.stdout)

    def test_missing_rocm_clang_tidy_is_an_error_not_a_clean_file(self) -> None:
        result = self._run("k.hip", HIP_CLANG_TIDY_BIN=str(self.tmp / "absent"))
        self.assertEqual(result.returncode, 127)
        self.assertRegex(result.stderr, r"^error: clang-tidy binary '.*absent' not found")


if __name__ == "__main__":
    unittest.main()
