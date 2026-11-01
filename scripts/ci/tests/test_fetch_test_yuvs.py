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

import functools
import hashlib
import io
import re
import shutil
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.http_stand_in import StandIn  # noqa: E402
from scripts.lib.safe_subprocess import TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

SCRIPT = ROOT / "scripts" / "test" / "fetch-test-yuvs.sh"
POLICY = "compat/python-vmaf/tools/retrying_download.py"
BASH = shutil.which("bash")
ZSTD = shutil.which("zstd")
FIRST = b"first fixture\n" * 32
SECOND = b"second fixture\n" * 32
BODIES = {"/yuv/first.yuv": FIRST, "/yuv/second.yuv": SECOND}
# The script's summary line: already-ok, replaced, fetched, failed.
SUMMARY = "fetch-test-yuvs.sh: {} already-ok, {} replaced, {} fetched, {} failed"


def run_bash(*args: str) -> TextCommandResult:
    """Run bash with args; bash is the only executable allowed."""
    assert BASH is not None
    return run_command(
        [BASH, *args],
        allowed_executables=[BASH],
        capture_output=True,
        text=True,
        timeout_seconds=120,
    )


def md5(data: bytes) -> str:
    return hashlib.md5(data, usedforsecurity=False).hexdigest()


def fetch(
    server: StandIn, directory: Path, *names: str, second_md5: str = md5(SECOND)
) -> TextCommandResult:
    """Source the script, point it at the stand-in server and run main()."""
    assert BASH is not None
    program = (
        'source "$1"; shift\n'
        f'FIXTURES=("first.yuv {md5(FIRST)}" "second.yuv {second_md5}")\n'
        'YUV_DIR="$1"; BASE_URL="$2"; shift 2\n'
        'main "$@"\n'
    )
    return run_bash("-c", program, "fetch", str(SCRIPT), str(directory), server.url("yuv"), *names)


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


SYNTHETIC_CREDIT = b"Source: synthetic credit\n"
SYNTHETIC_LICENCE = b"Licence: CC-BY-4.0\n"
SYNTHETIC_DIS = b"synthetic distorted 1080p yuv data\n" * 16
SYNTHETIC_REF = b"synthetic reference 1080p yuv data\n" * 16
SYNTHETIC_FILES = {
    "CREDIT.txt": SYNTHETIC_CREDIT,
    "LICENCE-CC-BY-4.0.txt": SYNTHETIC_LICENCE,
    "sollevante_dis_1920x1080_8f.yuv": SYNTHETIC_DIS,
    "sollevante_ref_1920x1080_8f.yuv": SYNTHETIC_REF,
}


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def make_tar_zst(
    members: dict[str, bytes], links: dict[str, tuple[bytes, str]] | None = None
) -> bytes:
    """A zstd tar of ``members``, plus ``links``: name -> (tar type, link target)."""
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode="w") as tf:
        for name, content in sorted(members.items()):
            ti = tarfile.TarInfo(name=name)
            ti.size = len(content)
            ti.mtime = 1761868800
            tf.addfile(ti, io.BytesIO(content))
        for name, (kind, target) in sorted((links or {}).items()):
            ti = tarfile.TarInfo(name=name)
            ti.type = kind
            ti.linkname = target
            ti.mtime = 1761868800
            tf.addfile(ti)
    assert ZSTD is not None
    proc = run_command(
        [ZSTD, "-q"],
        allowed_executables=[ZSTD],
        input_data=bio.getvalue(),
        capture_output=True,
        check=True,
    )
    assert isinstance(proc.stdout, bytes)
    return proc.stdout


@functools.cache
def synthetic_bundle() -> bytes:
    """The synthetic bundle archive, built on first use so a host without zstd only skips."""
    return make_tar_zst({f"pq-sollevante-v1/{k}": v for k, v in SYNTHETIC_FILES.items()})


SYNTHETIC_MEMBERS = [f"{k} {sha256_hex(v)}" for k, v in sorted(SYNTHETIC_FILES.items())]
BUNDLE_REQ_PATH = "/releases/pq-sollevante-v1.tar.zst"

SYNTHETIC_HDR_FILES = {
    "CREDIT.txt": b"Source: Netflix/vmaf_resource\n",
    "LICENCE-BSD-2-Clause-Patent.txt": b"Licence: BSD-2-Clause-Patent\n",
    "dis_1920x1080.yuv": b"synthetic dis 1080p yuv data\n" * 16,
    "dis_3840x2160.yuv": b"synthetic dis 2160p yuv data\n" * 16,
    "ref_1920x1080.yuv": b"synthetic ref 1080p yuv data\n" * 16,
    "ref_3840x2160.yuv": b"synthetic ref 2160p yuv data\n" * 16,
}
SYNTHETIC_HDR_BUNDLE_TAR_ZST = make_tar_zst(
    {f"hdr-ictcp-pixels-v1/{k}": v for k, v in SYNTHETIC_HDR_FILES.items()}
)
SYNTHETIC_HDR_BUNDLE_SHA256 = sha256_hex(SYNTHETIC_HDR_BUNDLE_TAR_ZST)
SYNTHETIC_HDR_MEMBERS = [f"{k} {sha256_hex(v)}" for k, v in sorted(SYNTHETIC_HDR_FILES.items())]
HDR_BUNDLE_REQ_PATH = "/releases/hdr-ictcp-pixels-v1.tar.zst"


def fetch_bundle(
    server: StandIn,
    directory: Path,
    *args: str,
    bundle_sha256: str | None = None,
    members: list[str] | None = None,
    path: str | None = None,
) -> TextCommandResult:
    """Source the script and run main() for bundle fetching."""
    assert BASH is not None
    if members is None:
        members = SYNTHETIC_MEMBERS
    if bundle_sha256 is None:
        bundle_sha256 = sha256_hex(synthetic_bundle())
    members_repr = " ".join(f'"{m}"' for m in members)
    program = (
        'source "$1"; shift\n'
        f'BUNDLE_ARCHIVE_SHA256="{bundle_sha256}"\n'
        f"BUNDLE_MEMBERS=({members_repr})\n"
        'YUV_DIR="$1"; BUNDLE_BASE_URL="$2"; shift 2\n'
        + (f'PATH="{path}"\n' if path else "")
        + 'main "$@"\n'
    )
    cli_args = args if args else ("--bundle", "pq-sollevante-v1")
    return run_bash(
        "-c",
        program,
        "fetch_bundle",
        str(SCRIPT),
        str(directory),
        server.url("releases"),
        *cli_args,
    )


@unittest.skipIf(
    BASH is None
    or shutil.which("sha256sum") is None
    or shutil.which("tar") is None
    or ZSTD is None,
    "needs bash, sha256sum, tar, and zstd",
)
class FetchBundleTest(unittest.TestCase):
    def test_bundle_fetch_positive(self) -> None:
        bodies = {BUNDLE_REQ_PATH: synthetic_bundle()}
        with tempfile.TemporaryDirectory() as tmp, StandIn(bodies=bodies) as server:
            done = fetch_bundle(server, Path(tmp))
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(server.requests, [BUNDLE_REQ_PATH])
            target_dir = Path(tmp) / "pq-sollevante-v1"
            self.assertTrue(target_dir.is_dir())
            for name, content in SYNTHETIC_FILES.items():
                self.assertEqual((target_dir / name).read_bytes(), content)
            self.assertIn("ok      bundle pq-sollevante-v1 (extracted and verified)", done.stdout)

    def test_bundle_archive_sha_mismatch_fails_and_leaves_nothing(self) -> None:
        bodies = {BUNDLE_REQ_PATH: synthetic_bundle()}
        with tempfile.TemporaryDirectory() as tmp, StandIn(bodies=bodies) as server:
            done = fetch_bundle(server, Path(tmp), bundle_sha256="0" * 64)
            self.assertEqual(done.returncode, 1)
            self.assertEqual(server.requests, [BUNDLE_REQ_PATH])
            target_dir = Path(tmp) / "pq-sollevante-v1"
            self.assertFalse(target_dir.exists())
            self.assertIn("archive sha256 mismatch", done.stderr)

    def test_bundle_traversal_refused_and_leaves_nothing(self) -> None:
        traversal_members = dict(SYNTHETIC_FILES)
        traversal_members["../evil.txt"] = b"traversal-data\n"
        evil_archive = make_tar_zst(traversal_members)
        evil_sha = sha256_hex(evil_archive)
        bodies = {BUNDLE_REQ_PATH: evil_archive}
        with tempfile.TemporaryDirectory() as tmp, StandIn(bodies=bodies) as server:
            done = fetch_bundle(server, Path(tmp), bundle_sha256=evil_sha)
            self.assertEqual(done.returncode, 1)
            self.assertEqual(server.requests, [BUNDLE_REQ_PATH])
            target_dir = Path(tmp) / "pq-sollevante-v1"
            self.assertFalse(target_dir.exists())
            self.assertFalse((Path(tmp) / "evil.txt").exists())
            self.assertIn("archive member contains illegal path or traversal", done.stderr)

    def _refuses_link(self, kind: bytes, target: str) -> None:
        """A bundle holding a link member of ``kind`` is refused and leaves nothing."""
        members = {f"pq-sollevante-v1/{k}": v for k, v in SYNTHETIC_FILES.items()}
        archive = make_tar_zst(members, {"pq-sollevante-v1/escape": (kind, target)})
        bodies = {BUNDLE_REQ_PATH: archive}
        with tempfile.TemporaryDirectory() as tmp, StandIn(bodies=bodies) as server:
            done = fetch_bundle(server, Path(tmp), bundle_sha256=sha256_hex(archive))
            self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
            self.assertFalse((Path(tmp) / "pq-sollevante-v1").exists())
            self.assertIn("archive member is not a regular file or directory", done.stderr)

    def test_bundle_symlink_member_refused(self) -> None:
        self._refuses_link(tarfile.SYMTYPE, "/etc/passwd")

    def test_bundle_hardlink_member_refused(self) -> None:
        self._refuses_link(tarfile.LNKTYPE, "pq-sollevante-v1/CREDIT.txt")

    def test_bundle_5xx_retried(self) -> None:
        bodies = {BUNDLE_REQ_PATH: synthetic_bundle()}
        with tempfile.TemporaryDirectory() as tmp, StandIn([503], bodies=bodies) as server:
            done = fetch_bundle(server, Path(tmp))
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(server.requests, [BUNDLE_REQ_PATH, BUNDLE_REQ_PATH])
            target_dir = Path(tmp) / "pq-sollevante-v1"
            self.assertTrue(target_dir.is_dir())
            self.assertIn("attempt 1 of 7 failed (HTTP Error 503", done.stderr)

    def test_bundle_404_fails_immediately_and_leaves_nothing(self) -> None:
        bodies = {BUNDLE_REQ_PATH: synthetic_bundle()}
        with tempfile.TemporaryDirectory() as tmp, StandIn([404], bodies=bodies) as server:
            done = fetch_bundle(server, Path(tmp))
            self.assertEqual(done.returncode, 1)
            self.assertEqual(server.requests, [BUNDLE_REQ_PATH])
            target_dir = Path(tmp) / "pq-sollevante-v1"
            self.assertFalse(target_dir.exists())
            self.assertIn("HTTP Error 404", done.stderr)

    def test_bundle_already_extracted_skips_download(self) -> None:
        bodies = {BUNDLE_REQ_PATH: synthetic_bundle()}
        with tempfile.TemporaryDirectory() as tmp, StandIn(bodies=bodies) as server:
            target_dir = Path(tmp) / "pq-sollevante-v1"
            target_dir.mkdir(parents=True)
            for name, content in SYNTHETIC_FILES.items():
                (target_dir / name).write_bytes(content)
            done = fetch_bundle(server, Path(tmp))
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(server.requests, [])
            self.assertIn("ok      bundle pq-sollevante-v1 (sha256 verified)", done.stdout)

    def test_unknown_bundle_name_refused_before_any_request(self) -> None:
        bodies = {BUNDLE_REQ_PATH: synthetic_bundle()}
        with tempfile.TemporaryDirectory() as tmp, StandIn(bodies=bodies) as server:
            done = fetch_bundle(server, Path(tmp), "--bundle", "unknown-bundle")
            self.assertEqual(server.requests, [])
            self.assertEqual(done.returncode, 2)
            self.assertIn("unknown-bundle is not a recognized bundle", done.stderr)

    def test_bundle_without_zstd_fails_before_any_request(self) -> None:
        """Negative: without zstd on PATH the script names it and downloads nothing."""
        bodies = {BUNDLE_REQ_PATH: synthetic_bundle()}
        with tempfile.TemporaryDirectory() as tmp, StandIn(bodies=bodies) as server:
            shim = Path(tmp) / "bin"
            shim.mkdir()
            for tool in ("mktemp", "mkdir", "rm", "dirname", "python3", "tar", "cat"):
                found = shutil.which(tool)
                if found:
                    (shim / tool).symlink_to(found)
            done = fetch_bundle(server, Path(tmp) / "yuv", path=str(shim))
            self.assertEqual(server.requests, [])
            self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
            self.assertIn("zstd is not on PATH", done.stderr)

    def test_print_bundle_sha_names_the_archive_sha(self) -> None:
        done = run_bash(str(SCRIPT), "--print-bundle-sha")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertRegex(done.stdout.strip(), r"^[0-9a-f]{64}$")
        # The default bundle is pq-sollevante-v1: the sha of its published release asset.
        self.assertEqual(
            done.stdout.strip(),
            "f7546a16c3190c841e09a5de83d98db86156a06869fb9fbc58ff86307db2f416",
        )

    def test_hdr_bundle_fetch_positive(self) -> None:
        bodies = {HDR_BUNDLE_REQ_PATH: SYNTHETIC_HDR_BUNDLE_TAR_ZST}
        with tempfile.TemporaryDirectory() as tmp, StandIn(bodies=bodies) as server:
            done = fetch_bundle(
                server,
                Path(tmp),
                "--bundle",
                "hdr-ictcp-pixels-v1",
                bundle_sha256=SYNTHETIC_HDR_BUNDLE_SHA256,
                members=SYNTHETIC_HDR_MEMBERS,
            )
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(server.requests, [HDR_BUNDLE_REQ_PATH])
            target_dir = Path(tmp) / "hdr-ictcp-pixels-v1"
            self.assertTrue(target_dir.is_dir())
            for name, content in SYNTHETIC_HDR_FILES.items():
                self.assertEqual((target_dir / name).read_bytes(), content)
            self.assertIn(
                "ok      bundle hdr-ictcp-pixels-v1 (extracted and verified)", done.stdout
            )

    def test_hdr_bundle_archive_sha_mismatch_fails_and_leaves_nothing(self) -> None:
        bodies = {HDR_BUNDLE_REQ_PATH: SYNTHETIC_HDR_BUNDLE_TAR_ZST}
        with tempfile.TemporaryDirectory() as tmp, StandIn(bodies=bodies) as server:
            done = fetch_bundle(
                server,
                Path(tmp),
                "--bundle",
                "hdr-ictcp-pixels-v1",
                bundle_sha256="0" * 64,
                members=SYNTHETIC_HDR_MEMBERS,
            )
            self.assertEqual(done.returncode, 1)
            self.assertEqual(server.requests, [HDR_BUNDLE_REQ_PATH])
            target_dir = Path(tmp) / "hdr-ictcp-pixels-v1"
            self.assertFalse(target_dir.exists())
            self.assertIn("archive sha256 mismatch", done.stderr)
            self.assertIn("hdr-ictcp-pixels-v1", done.stderr)

    def test_print_bundle_sha_for_hdr_bundle(self) -> None:
        done = run_bash(str(SCRIPT), "--print-bundle-sha", "hdr-ictcp-pixels-v1")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(
            done.stdout.strip(),
            "b5cc0aff25a507fccd4808c8c7c4692db3bdf1a68bd3e77c6f081c5398d4ab1a",
        )

    def test_print_bundle_sha_unknown_bundle_refused(self) -> None:
        done = run_bash(str(SCRIPT), "--print-bundle-sha", "unknown-bundle")
        self.assertEqual(done.returncode, 2)
        self.assertIn("unknown-bundle is not a recognized bundle", done.stderr)

    def test_print_bundle_sha_empty_name_refused(self) -> None:
        """Negative: an empty name is refused, not read as the default bundle."""
        done = run_bash(str(SCRIPT), "--print-bundle-sha", "")
        self.assertEqual(done.returncode, 2)
        self.assertIn("is not a recognized bundle", done.stderr)

    def test_empty_bundle_option_refused(self) -> None:
        """Negative: --bundle with an empty name stops instead of fetching every fixture."""
        done = run_bash(str(SCRIPT), "--bundle", "")
        self.assertEqual(done.returncode, 2)
        self.assertIn("--bundle requires a bundle name", done.stderr)

    def test_empty_bundle_equals_option_refused(self) -> None:
        """Negative: --bundle= with an empty name stops instead of fetching every fixture."""
        done = run_bash(str(SCRIPT), "--bundle=")
        self.assertEqual(done.returncode, 2)
        self.assertIn("--bundle requires a bundle name", done.stderr)
        self.assertNotIn("fetch ", done.stdout)


# "${a[@]}" of an empty array is an unbound variable under set -u in Bash before 4.4,
# which macOS runners ship (3.2); ${a[@]+"${a[@]}"} expands to nothing there instead.
UNGUARDED_ARRAY = re.compile(r'(?<!\[@\]\+)"\$\{[A-Za-z_][A-Za-z0-9_]*\[@\]\}"')


def unguarded_array_expansions(text: str) -> list[int]:
    """Line numbers holding a "${a[@]}" expansion without the ${a[@]+...} guard."""
    return [n for n, line in enumerate(text.splitlines(), 1) if UNGUARDED_ARRAY.search(line)]


class OldBashArrays(unittest.TestCase):
    """The fetch script runs on macOS's Bash 3.2, so every array expansion is guarded."""

    def test_the_script_guards_every_array_expansion(self) -> None:
        self.assertEqual(unguarded_array_expansions(SCRIPT.read_text()), [])

    def test_an_unguarded_expansion_is_found(self) -> None:
        text = 'ok ${A[@]+"${A[@]}"}\nf "${PARSED_FIXTURES[@]}"\n'
        self.assertEqual(unguarded_array_expansions(text), [2])


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


def _unit_suite_gets_the_pair(workflow: str) -> bool:
    """The coverage job fetches the pair before the step that requires it."""
    fetch = workflow.find("Fetch the Netflix golden YUV fixtures for the unit suite")
    suite = workflow.find("id: coverage_meson_suite")
    required = workflow.find("VMAFX_REQUIRE_NETFLIX_PAIR", suite)
    next_step = workflow.find("- name:", suite)
    return -1 < fetch < suite < required < next_step


class PythonBindingBitExactRuns(unittest.TestCase):
    """The bit-exact test of the Python binding cannot skip in CI (#2434)."""

    WORKFLOW = ROOT / ".github" / "workflows" / "tests-and-quality-gates.yml"

    def test_the_unit_suite_requires_the_pair_it_fetched(self) -> None:
        self.assertTrue(_unit_suite_gets_the_pair(self.WORKFLOW.read_text(encoding="utf-8")))

    def test_a_workflow_without_the_requirement_is_refused(self) -> None:
        text = self.WORKFLOW.read_text(encoding="utf-8")
        self.assertFalse(_unit_suite_gets_the_pair(text.replace("VMAFX_REQUIRE_NETFLIX_PAIR", "X")))

    def test_a_workflow_without_the_fetch_is_refused(self) -> None:
        text = self.WORKFLOW.read_text(encoding="utf-8")
        self.assertFalse(
            _unit_suite_gets_the_pair(text.replace("YUV fixtures for the unit suite", "X"))
        )


if __name__ == "__main__":
    unittest.main()


def test_every_tox_leg_fetches_the_pq_fixture_first() -> None:
    """A tox leg without the bundle asks Netflix for it and fails with HTTP 404."""
    workflow = ROOT / ".github" / "workflows" / "libvmaf-build-matrix.yml"
    text = workflow.read_text(encoding="utf-8")
    fetch = text.index("name: Fetch the PQ fixture")
    assert fetch < text.index("name: Run tox tests (ubuntu)")
    assert fetch < text.index("name: Run tox tests (mac)")
    assert text.index("name: Run tox tests (mac)") < text.index("name: Put away the PQ fixture")
    assert text.index("name: Put away the PQ fixture") < text.index(
        "name: Save Netflix vmaf_resource fixtures"
    )


def test_every_tox_leg_fetches_the_hdr_fixture_too() -> None:
    """A tox leg without the HDR bundle fails on missing converted pixels."""
    workflow = ROOT / ".github" / "workflows" / "libvmaf-build-matrix.yml"
    text = workflow.read_text(encoding="utf-8")
    fetch = text.index("name: Fetch the HDR ICtCp fixture")
    assert fetch < text.index("name: Run tox tests (ubuntu)")
    assert fetch < text.index("name: Run tox tests (mac)")
    assert text.index("name: Run tox tests (mac)") < text.index(
        "name: Put away the HDR ICtCp fixture"
    )
