"""Exercise the real smoke Git operations without a native FFmpeg build."""

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci import ffmpeg_shared_series_fixture as shared_series
from scripts.lib.safe_subprocess import TextCommandResult
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[2]
SMOKE = ROOT / "ffmpeg-patches/test/build-and-run.sh"
TAG_CHECKOUT = ROOT / "scripts/ci/checkout-annotated-tag.sh"
SHARED_SERIES = ROOT / "scripts/ci/ffmpeg-shared-series.sh"
CONSUMER_LIB = ROOT / "scripts/ci/upstream-consumer-lib.sh"
CONSUMER_SCORES = ROOT / "scripts/ci/upstream_consumer_scores.py"
SCORE = (
    '{{"frames": [{{"frameNum": 0, "metrics": {{"vmaf": {value}}}}}], '
    '"pooled_metrics": {{"vmaf": {{"mean": {value}}}}}}}\n'
)


class SmokeSafety(unittest.TestCase):
    def git(self, path: Path, *args: str) -> str:
        # Isolate setup and assertions before injecting fake caller variables
        # into the smoke script itself. -C alone cannot select the fixture.
        environment = {
            key: value for key, value in os.environ.items() if not key.startswith("GIT_")
        }
        environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        git = shutil.which("git")
        self.assertIsNotNone(git)
        assert git is not None
        result = run_command(
            [
                git,
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
            allowed_executables=(git,),
            capture_output=True,
            text=True,
            env=environment,
            check=True,
            timeout_seconds=60,
        )
        assert isinstance(result.stdout, str)
        return result.stdout

    def init_repositories(self) -> None:
        for path in (self.upstream, self.caller):
            path.mkdir()
            self.git(path, "init", "-q", "-b", "master")
            (path / "sample").write_text("before\n")
            self.git(path, "add", "sample")
            self.git(path, "commit", "-qm", "initial fixture")

    def create_upstream_patch(self) -> str:
        configure = self.upstream / "configure"
        # The fake configure records its arguments; the fake ffmpeg answers the
        # option probes and, for a scoring graph, writes the score file the
        # test hands it in SMOKE_FAKE_FFMPEG_JSON.
        configure.write_text(
            "#!/bin/sh\nprintf '%s\\n' \"$@\" > configure.args\n"
            "cat > ffmpeg <<'BINARY'\n#!/bin/sh\n"
            'case "$*" in\n*log_fmt=json*) cat "$SMOKE_FAKE_FFMPEG_JSON" > ffmpeg.json;;\n'
            "*filter=libvmaf*) echo tiny_model;;\n"
            "*filter=vmaf_pre*) exit 0;;\n*) exit 1;;\nesac\nBINARY\n"
            "chmod +x ffmpeg\n"
        )
        configure.chmod(0o755)
        self.git(self.upstream, "add", "configure")
        self.git(self.upstream, "commit", "-qm", "fake native configure")
        self.git(self.upstream, "tag", "-a", "n9.0.1", "-m", "annotated fixture release")
        # The shared fix series (ADR-3143): one patch on the release, applied
        # before the integration patch and independent of it.
        (self.upstream / "shared-fix").write_text("fixed upstream of the series\n")
        self.git(self.upstream, "add", "shared-fix")
        self.git(self.upstream, "commit", "-qm", "shared fix")
        self.shared_patch = self.git(self.upstream, "format-patch", "-1", "--stdout")
        self.git(self.upstream, "reset", "-q", "--hard", "n9.0.1")
        (self.upstream / "sample").write_text("patched\n")
        self.git(self.upstream, "commit", "-qam", "integration patch")
        return self.git(self.upstream, "format-patch", "-1", "--stdout")

    def create_project_fixture(self, patch: str) -> None:
        self.project = self.root / "fixture"
        patches = self.project / "ffmpeg-patches"
        (patches / "test").mkdir(parents=True)
        self.script = patches / "test/build-and-run.sh"
        shutil.copy2(SMOKE, self.script)
        helper_dir = self.project / "scripts/ci"
        helper_dir.mkdir(parents=True)
        for helper in (TAG_CHECKOUT, CONSUMER_LIB, CONSUMER_SCORES, SHARED_SERIES):
            shutil.copy2(helper, helper_dir / helper.name)
        testdata = self.project / "testdata"
        testdata.mkdir()
        for name in ("ref_576x324_48f.yuv", "dis_576x324_48f.yuv"):
            (testdata / name).write_bytes(bytes(576 * 324 * 3 // 2))
        (patches / "0001-fixture.patch").write_text(patch)
        (patches / "series.txt").write_text("0001-fixture.patch\n")
        base = self.git(self.upstream, "rev-parse", "n9.0.1^{commit}").strip()
        archive, digest = shared_series.tarball(
            self.root / "shared-series",
            "n9.0.1",
            base,
            {shared_series.PATCH_NAME: self.shared_patch},
        )
        (self.project / "build-config.env").write_text(
            f'FFMPEG_REMOTE="{self.upstream}"\nFFMPEG_TAG="n9.0.1"\nFFMPEG_COMMIT="{base}"\n'
            + shared_series.pins(archive, digest)
        )

    def create_fake_tools(self) -> None:
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for name, body in (
            ("pkg-config", "exit 0"),
            (
                "make",
                # The build step prints what a test hands it, as the compiler would.
                'case " $* " in *" build "*) printf \'%s\' "${SMOKE_FAKE_BUILD_OUTPUT:-}";; esac\n'
                'if [ "$*" = "-s fate-list" ]; then '
                "printf 'GEN\\ttests/generated.mak\\nfate-fixture\\n'; fi\nexit 0",
            ),
            ("nproc", "echo 1"),
        ):
            path = self.bin / name
            path.write_text("#!/bin/sh\n" + body + "\n")
            path.chmod(0o755)

    def configure_caller_environment(self) -> None:
        (self.caller / "unique-staged-work").write_text("preserve this staging\n")
        self.git(self.caller, "add", "unique-staged-work")
        self.before_head = self.git(self.caller, "rev-parse", "HEAD")
        self.before_index = (self.caller / ".git/index").read_bytes()
        self.checkout = self.root / "ffmpeg-checkout"
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        self.env.update(
            {
                "GIT_CONFIG_NOSYSTEM": "1",
                "GIT_CONFIG_GLOBAL": os.devnull,
                "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
                "FFMPEG_SRC": str(self.checkout),
                "KEEP_BUILD": "1",
                # Off the user's cache: the stand-in series is this fixture's.
                "FFMPEG_FIX_SERIES_CACHE": str(self.root / "fix-series-cache"),
            }
        )

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="ffmpeg-smoke-safety-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.upstream = self.root / "upstream"
        self.caller = self.root / "caller"
        self.init_repositories()
        self.create_project_fixture(self.create_upstream_patch())
        self.create_fake_tools()
        self.configure_caller_environment()

    def run_smoke(self) -> TextCommandResult:
        # Fixed Bash executable, copied repository script and fixture-owned cwd.
        bash = shutil.which("bash")
        self.assertIsNotNone(bash)
        assert bash is not None
        result = run_command(
            [bash, str(self.script)],
            allowed_executables=(bash,),
            cwd=self.project,
            env=self.env,
            text=True,
            capture_output=True,
            timeout_seconds=30,
        )
        # A smoke run that succeeded must also have been quiet: this branch makes
        # FFmpeg diagnostics fatal, so a passing exit code with a warning in the
        # output would mean the gate it adds is not actually reading anything.
        if result.returncode == 0:
            assert isinstance(result.stdout, str) and isinstance(result.stderr, str)
            self.assertNotRegex(result.stdout + result.stderr, r"(?i)warning:|error:")
        return result

    def test_shared_fix_series_is_applied_before_the_integration_series(self) -> None:
        result = self.run_smoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            (self.checkout / "shared-fix").read_text(), "fixed upstream of the series\n"
        )
        subjects = self.git(self.checkout, "log", "--reverse", "--format=%s", "n9.0.1..HEAD")
        self.assertEqual(subjects.splitlines(), ["shared fix", "integration patch"])

    def test_tampered_shared_series_stops_the_smoke_run(self) -> None:
        config = self.project / "build-config.env"
        text = config.read_text()
        digest = re.search(r'FFMPEG_FIX_SERIES_SHA256="([0-9a-f]{64})"', text)
        assert digest is not None
        config.write_text(text.replace(digest.group(1), "0" * 64))
        result = self.run_smoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("build-config.env pins", result.stderr)
        # Nothing of the integration series was applied either.
        self.assertEqual((self.checkout / "sample").read_text(), "before\n")

    def run_with_build_output(self, toolchain: str, output: str) -> TextCommandResult:
        """One smoke run, in a fresh checkout, whose build step prints `output`."""
        self.runs = getattr(self, "runs", 0) + 1
        self.env.update(
            FFMPEG_SRC=str(self.root / f"ffmpeg-build-{self.runs}"),
            FFMPEG_TOOLCHAIN=toolchain,
            SMOKE_FAKE_BUILD_OUTPUT=output,
        )
        return self.run_smoke()

    def configured_args(self) -> list[str]:
        return (self.checkout / "configure.args").read_text().splitlines()

    def install_fake_vmaf(self, cli_value: str, ffmpeg_value: str) -> None:
        """A prefix whose bin/vmaf writes cli_value; the fake ffmpeg writes ffmpeg_value."""
        prefix = self.root / "prefix"
        (prefix / "lib/pkgconfig").mkdir(parents=True)
        (prefix / "lib/pkgconfig/libvmaf.pc").write_text("Name: libvmaf\n")
        (prefix / "bin").mkdir()
        cli = prefix / "bin/vmaf"
        cli.write_text(
            "#!/bin/sh\nout=\n"
            'while [ "$#" -gt 0 ]; do [ "$1" = -o ] && out="$2"; shift; done\n'
            f"cat > \"$out\" <<'JSON'\n{SCORE.format(value=cli_value)}JSON\n"
        )
        cli.chmod(0o755)
        ffmpeg_json = self.root / "ffmpeg-score.json"
        ffmpeg_json.write_text(SCORE.format(value=ffmpeg_value))
        self.env.update(
            VMAF_PREFIX=str(prefix),
            VMAF_SCORE_CHECK="1",
            SMOKE_FAKE_FFMPEG_JSON=str(ffmpeg_json),
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

    def test_crlf_series_applies(self) -> None:
        series = self.project / "ffmpeg-patches/series.txt"
        series.write_bytes(series.read_bytes().replace(b"\n", b"\r\n"))
        result = self.run_smoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.checkout / "sample").read_text(), "patched\n")

    def test_default_build_passes_no_toolchain(self) -> None:
        result = self.run_smoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(any(a.startswith("--toolchain") for a in self.configured_args()))
        self.assertIn("Running generated FATE subset", result.stdout)

    def test_msvc_toolchain_reaches_configure_with_the_dll_runtime(self) -> None:
        self.env["FFMPEG_TOOLCHAIN"] = "msvc"
        result = self.run_smoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        args = self.configured_args()
        for expected in ("--toolchain=msvc", "--extra-cflags=-MD", "--extra-cxxflags=-MD"):
            self.assertIn(expected, args)
        self.assertIn("--enable-libvmaf", args)

    def test_msvc_gate_leaves_ffmpeg_own_warnings_out(self) -> None:
        # positive: cl's warnings in files the series does not touch, including
        # one whose name only ends like a series file, on a line of a series
        # file the series did not write (the fixture patch changes line 1 of
        # `sample`), and D9024 from FFmpeg's host-tool links.
        result = self.run_with_build_output(
            "msvc",
            "libavfilter/vf_overlay.c(582): warning C4334: '<<': result of 32-bit shift\n"
            "libavfilter\\notsample(1): warning C4101: 'x': unreferenced local variable\n"
            "sample(7): warning C4334: '<<': result of 32-bit shift\n"
            ".\\sample(17,3): warning C4334: '<<': result of 32-bit shift\n"
            "cl : Command line warning D9024 : unrecognized source file type 'tests/base64.o'\n",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("FFmpeg emitted compiler warnings", result.stderr)

    def test_msvc_gate_refuses_warnings_in_series_files(self) -> None:
        # negative and boundary: a line the series wrote, under each path form
        # cl prints.
        for line in (
            "sample(1): warning C4133: 'function': incompatible types",
            ".\\sample(1): warning C4133: 'function': incompatible types",
            "C:\\build\\ffmpeg\\sample(1,5): warning C4133: 'function': incompatible types",
        ):
            with self.subTest(line=line):
                result = self.run_with_build_output("msvc", line + "\n")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("FFmpeg emitted compiler warnings", result.stderr)
                self.assertIn(line, result.stderr)

    def test_msvc_gate_refuses_linker_warnings(self) -> None:
        line = "vmaf.lib(a.obj) : warning LNK4098: defaultlib 'LIBCMT' conflicts with use of other libs"
        result = self.run_with_build_output("msvc", line + "\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(line, result.stderr)

    def test_default_gate_refuses_every_warning(self) -> None:
        line = "libavfilter/vf_overlay.c:582:5: warning: shift count is too large"
        result = self.run_with_build_output("", line + "\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FFmpeg emitted compiler warnings", result.stderr)
        self.assertIn(line, result.stderr)

    def test_unknown_settings_are_refused_before_clone(self) -> None:
        for name, value in (
            ("FFMPEG_TOOLCHAIN", "gcc"),
            ("SMOKE_FATE", "yes"),
            ("VMAF_SCORE_CHECK", "2"),
            ("FFMPEG_JOBS", "0"),
            ("FFMPEG_JOBS", "1000"),
        ):
            with self.subTest(name=name, value=value):
                env = dict(self.env, **{name: value})
                saved, self.env = self.env, env
                try:
                    result = self.run_smoke()
                finally:
                    self.env = saved
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(name, result.stderr)
                self.assertFalse(self.checkout.exists(), result.stdout + result.stderr)

    def test_score_check_needs_a_prefix(self) -> None:
        self.env["VMAF_SCORE_CHECK"] = "1"
        result = self.run_smoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("needs VMAF_PREFIX", result.stderr)
        self.assertFalse(self.checkout.exists())

    def test_fate_skip_is_stated(self) -> None:
        self.env["SMOKE_FATE"] = "0"
        result = self.run_smoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SKIP: FATE subset not run (SMOKE_FATE=0)", result.stdout)
        self.assertNotIn("Running generated FATE subset", result.stdout)

    def test_matching_filter_and_cli_scores_pass(self) -> None:
        self.install_fake_vmaf("76.668905", "76.668905")
        result = self.run_smoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: ffmpeg-patches smoke ok", result.stdout)
        self.assertTrue((self.checkout / "vmafx-score/ffmpeg.json").is_file())

    def test_last_digit_score_difference_fails(self) -> None:
        self.install_fake_vmaf("76.668905", "76.668906")
        result = self.run_smoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("PASS: ffmpeg-patches smoke ok", result.stdout)
        self.assertIn("76.668906", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
