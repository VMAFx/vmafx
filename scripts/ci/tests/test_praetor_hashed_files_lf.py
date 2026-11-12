#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Files hashed or compared byte for byte check out with LF everywhere.

`* text=auto` in .gitattributes checks a text file out with core.eol, whose
default is the platform's line ending. On Windows that gave CRLF even with
core.autocrlf=false, and the pinned praetorctl reads these files as raw bytes:
the archetype digest no longer matched .standards.lock and compile-context
found CLAUDE.md out of sync with AGENTS.md
(T-WINDOWS-CRLF-PRAETOR-HASHED-FILES-2026-10-06). The test checks each file out
the way a Windows host does (core.eol=crlf) and requires the bytes of the
commit, so it fails for any such file without an `eol=lf` rule.

The VMAFx model files are the second group: the build embeds each built-in
model byte for byte (`xxd -i` in core/src/meson.build) and vmafx_model_hash()
is the SHA-256 of the bytes as loaded (docs/api/vmafx/index.md), so a CRLF checkout gave every
Windows build and every model file loaded there another hash
(T-WINDOWS-CRLF-MODEL-HASH-2026-10-08).

The compiled agent-context projections (`praetorctl compile-context` writes six
bannered targets plus `AGENTS.readonly.md` from `AGENTS.md`) also carry
`linguist-generated` in `.gitattributes` while `AGENTS.md` stays unmarked.
"""

from __future__ import annotations

import hashlib
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.safe_subprocess import BinaryCommandResult, TextCommandResult
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
GIT = shutil.which("git") or "/usr/bin/git"
TIMEOUT_SECONDS = 120.0
WINDOWS_CHECKOUT = ("-c", "core.autocrlf=false", "-c", "core.eol=crlf")
MODEL_DIR = "model/"
COMPILED_BANNER = "Compiled automatically by praetorctl compile-context"
READONLY_PROJECTION = "AGENTS.readonly.md"
TEST_TREE_SEGMENTS = frozenset(("tests", "testdata"))
CHECK_ATTR_FIELDS = 3
ATTR_VALUE_INDEX = 2
MAX_ATTR_RECORDS = 1024
MIN_BANNER_TARGETS = 6

# What the pinned engine reads as raw bytes, by its source
# (cordanaLLM/praetor at the standards-gate.yml PRAETOR_REF):
# internal/config/lockdigest.go hashes .config/archetypes; the effective policy
# digest reads .standards.yaml and .standards.lock; internal/agentcontext/render.go
# lists the compiled targets and their persona directories,
# internal/agentcontext/readonly.go names the read-only projection, and
# internal/compiler/agent_projection.go the persona sources under .agents/.
EXACT = (
    "AGENTS.md",
    "AGENTS.readonly.md",
    "CLAUDE.md",
    ".windsurfrules",
    ".github/copilot-instructions.md",
    ".gemini/GEMINI.md",
    ".codex/rules.md",
    ".standards.yaml",
    ".standards.lock",
    ".standards-baseline.json",
)
PREFIXES = (
    ".cursor/rules/",
    ".agents/",
    ".claude/agents/",
    ".github/agents/",
    ".gemini/agents/",
    ".codex/agents/",
    ".config/archetypes/",
)


def git_text(*args: str) -> TextCommandResult:
    return run_command(
        [GIT, *args],
        allowed_executables=(GIT,),
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=TIMEOUT_SECONDS,
    )


def git_bytes(*args: str) -> BinaryCommandResult:
    return run_command(
        [GIT, *args],
        allowed_executables=(GIT,),
        cwd=ROOT,
        capture_output=True,
        check=True,
        timeout_seconds=TIMEOUT_SECONDS,
    )


def hashed_files() -> list[str]:
    tracked = git_text("ls-files", "-z").stdout.split("\0")
    return sorted(
        path
        for path in tracked
        if path in EXACT or (path.startswith(PREFIXES) and not path.endswith("/"))
    )


def model_files() -> list[str]:
    tracked = git_text("ls-files", "-z").stdout.split("\0")
    return sorted(path for path in tracked if path.startswith(MODEL_DIR) and path.endswith(".json"))


def built_in_models() -> list[str]:
    """The model file names core/src/meson.build embeds into the library."""
    build = (ROOT / "core" / "src" / "meson.build").read_text(encoding="utf-8")
    return sorted(set(re.findall(r"'(vmaf_[^'/]+\.json)'", build)))


def windows_checkout(paths: list[str], prefix: Path) -> None:
    """Write paths under prefix with the conversion a Windows host applies."""
    git_text(*WINDOWS_CHECKOUT, "checkout-index", f"--prefix={prefix.as_posix()}/", "--", *paths)


def converted_on_windows(paths: list[str]) -> list[str]:
    """The paths whose Windows checkout differs from the committed blob."""
    with tempfile.TemporaryDirectory(prefix="lf-") as scratch:
        prefix = Path(scratch)
        windows_checkout(paths, prefix)
        return [
            path
            for path in paths
            if (prefix / path).read_bytes() != git_bytes("cat-file", "blob", f":{path}").stdout
        ]


def is_test_tree_path(path: str) -> bool:
    """True when a repository-relative path sits inside a tests or testdata directory."""
    return bool(TEST_TREE_SEGMENTS.intersection(path.split("/")[:-1]))


def banner_grep_paths() -> list[str]:
    """Every tracked path carrying the compile-context banner."""
    raw = git_text("grep", "-l", "-z", "-F", COMPILED_BANNER).stdout.split("\0")
    return sorted(path for path in raw[:MAX_ATTR_RECORDS] if path)


def compiled_banner_targets(candidates: list[str] | None = None) -> list[str]:
    """Banner-carrying paths outside test and testdata trees."""
    paths = banner_grep_paths() if candidates is None else candidates
    return sorted(path for path in paths[:MAX_ATTR_RECORDS] if not is_test_tree_path(path))


def linguist_generated_attrs(paths: list[str]) -> dict[str, str]:
    """Map each path to its linguist-generated attribute state from git check-attr."""
    if not paths:
        return {}
    fields = git_text("check-attr", "-z", "linguist-generated", "--", *paths).stdout.split("\0")
    usable = fields[: MAX_ATTR_RECORDS * CHECK_ATTR_FIELDS]
    complete = len(usable) - (len(usable) % CHECK_ATTR_FIELDS)
    return {
        usable[idx]: usable[idx + ATTR_VALUE_INDEX] for idx in range(0, complete, CHECK_ATTR_FIELDS)
    }


class PraetorHashedFilesCheckOutLf(unittest.TestCase):
    def test_every_group_has_files(self) -> None:
        files = hashed_files()
        for exact in EXACT:
            self.assertIn(exact, files, f"{exact} is not tracked; update the test with praetor")
        for prefix in PREFIXES:
            self.assertTrue(
                any(path.startswith(prefix) for path in files),
                f"no tracked file under {prefix}; update the test with praetor",
            )

    def test_windows_checkout_keeps_the_committed_bytes(self) -> None:
        converted = converted_on_windows(hashed_files())
        self.assertEqual(converted, [], "a Windows checkout rewrites these files; add eol=lf")

    def test_windows_checkout_of_each_archetype_hashes_to_its_pin(self) -> None:
        lock = (ROOT / ".standards.lock").read_text(encoding="utf-8")
        pins = re.findall(r"sha256:([0-9a-f]{64})", lock)
        archetypes = [path for path in hashed_files() if path.startswith(".config/archetypes/")]
        with tempfile.TemporaryDirectory(prefix="praetor-lf-") as scratch:
            prefix = Path(scratch)
            windows_checkout(archetypes, prefix)
            digests = {
                path: hashlib.sha256((prefix / path).read_bytes()).hexdigest()
                for path in archetypes
            }
        unpinned = sorted(path for path, digest in digests.items() if digest not in pins)
        self.assertEqual(unpinned, [], "the .standards.lock pin no longer matches on Windows")

    def test_checkout_conversion_is_observable(self) -> None:
        """Negative case: the emulated Windows checkout does convert a plain text file."""
        sample = "docs/index.md"
        with tempfile.TemporaryDirectory(prefix="praetor-lf-") as scratch:
            prefix = Path(scratch)
            windows_checkout([sample], prefix)
            self.assertIn(b"\r\n", (prefix / sample).read_bytes())

    def test_compiled_context_projections_are_linguist_generated(self) -> None:
        """Positive and negative checks for linguist-generated on context projections."""
        targets = compiled_banner_targets()
        self.assertGreaterEqual(len(targets), MIN_BANNER_TARGETS)
        generated = sorted({*targets, READONLY_PROJECTION})
        attrs = linguist_generated_attrs([*generated, "AGENTS.md"])
        self.assertEqual(
            [path for path in generated if attrs.get(path) != "set"],
            [],
            "every compiled context projection needs linguist-generated in .gitattributes",
        )
        self.assertEqual(attrs.get("AGENTS.md"), "unspecified")

    def test_banner_quoting_test_files_are_excluded_by_path_rule(self) -> None:
        """Boundary: test files under scripts/ci/tests/ quoting the banner are excluded."""
        test_matches = [
            path for path in banner_grep_paths() if path.startswith("scripts/ci/tests/")
        ]
        self.assertTrue(test_matches, "expected banner-quoting test files under scripts/ci/tests/")
        targets = compiled_banner_targets()
        for path in test_matches[:MAX_ATTR_RECORDS]:
            self.assertNotIn(path, targets)
        attrs = linguist_generated_attrs(test_matches)
        self.assertEqual(
            [path for path in test_matches if attrs.get(path) == "set"],
            [],
        )
        probe = "scripts/ci/tests/banner_fixture.md"
        self.assertEqual(compiled_banner_targets(["CLAUDE.md", probe]), ["CLAUDE.md"])


class ModelFilesCheckOutLf(unittest.TestCase):
    def test_every_built_in_model_is_a_tracked_model_file(self) -> None:
        names = {Path(path).name for path in model_files()}
        built_in = built_in_models()
        self.assertGreaterEqual(len(built_in), 9, "the built-in list in core/src/meson.build moved")
        self.assertEqual([name for name in built_in if name not in names], [])

    def test_windows_checkout_keeps_the_committed_bytes(self) -> None:
        converted = converted_on_windows(model_files())
        self.assertEqual(converted, [], "a Windows checkout rewrites these models; add eol=lf")


if __name__ == "__main__":
    unittest.main()
