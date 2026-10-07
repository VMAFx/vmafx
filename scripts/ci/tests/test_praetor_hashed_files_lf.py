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

# What the pinned engine reads as raw bytes, by its source
# (cordanaLLM/praetor at the standards-gate.yml PRAETOR_REF):
# internal/config/lockdigest.go hashes .config/archetypes; the effective policy
# digest reads .standards.yaml and .standards.lock; internal/agentcontext/render.go
# lists the compiled targets and their persona directories, and
# internal/compiler/agent_projection.go the persona sources under .agents/.
EXACT = (
    "AGENTS.md",
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
