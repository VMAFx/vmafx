#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract tests ensuring Makefile governance targets run the pinned Praetor engine (#3468)."""

from __future__ import annotations

import os
import re
import shutil
import sys
import unittest
from pathlib import Path
from typing import ClassVar

IMPORT_ROOT = Path(__file__).resolve().parents[3]
if str(IMPORT_ROOT) not in sys.path:
    sys.path.insert(0, str(IMPORT_ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

REPO_ROOT = IMPORT_ROOT
MAKEFILE = REPO_ROOT / "Makefile"
INCLUDE_LINE = "-include .config/praetor/engine.mk"
SECTION_HEADER = "# cordanaLLM/praetor Governance Targets"
GOVERNANCE_TARGETS: tuple[str, ...] = (
    "verify-all",
    "compile-context",
    "audit",
    "hiss-coverage",
    "dedupe-check",
)
LITERAL_ENGINES: frozenset[str] = frozenset({"standardsctl", "praetorctl"})
ENGINE_VARS: tuple[str, ...] = (
    "$(PRAETORCTL)",
    "${PRAETORCTL}",
    "$(PRAETOR_ENGINE)",
    "${PRAETOR_ENGINE}",
)
MAX_MAKEFILE_LINES = 65_536
MAX_RECIPE_LINES = 256
MAX_RECIPE_CLAUSES = 32
MAX_CLAUSE_WORDS = 64
MAKE_TIMEOUT_SECONDS = 60.0
_CLAUSE_SPLIT_RE = re.compile(r"\s*(?:&&|\|\||;|\|)\s*")
_ENV_ASSIGN_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")


def _first_command_word(clause: str) -> str:
    """Return the command word of a shell clause after optional env assignments."""
    words = clause.split()[:MAX_CLAUSE_WORDS]
    for idx, word in enumerate(words):
        if idx == 0 and word == "env":
            continue
        if _ENV_ASSIGN_RE.match(word):
            continue
        return word
    return ""


def _check_clause(target: str, lineno: int, line_ok: bool, clause: str) -> tuple[int, list[str]]:
    """Validate one shell clause inside a governance recipe line."""
    cmd_word = _first_command_word(clause)
    if not cmd_word:
        return 0, []
    if cmd_word.rsplit("/", 1)[-1] in LITERAL_ENGINES:
        return 0, [f"line {lineno} ({target}): literal engine call {cmd_word!r}; use $(PRAETORCTL)"]
    if any(var in clause for var in ENGINE_VARS):
        if not line_ok or not clause.startswith("$(PRAETORCTL) "):
            return 0, [
                f"line {lineno} ({target}): engine call must start with @$(PRAETORCTL) or $(PRAETORCTL)"
            ]
        return 1, []
    return 0, []


def _check_recipe_line(target: str, lineno: int, body: str) -> tuple[int, list[str]]:
    """Validate one tab-indented recipe line for a governance target."""
    after_prefix = body.lstrip().lstrip("@-+").lstrip()
    if not after_prefix or after_prefix.startswith("#"):
        return 0, []
    line_ok = body.startswith(("@$(PRAETORCTL) ", "$(PRAETORCTL) "))
    calls = 0
    findings: list[str] = []
    for clause in _CLAUSE_SPLIT_RE.split(after_prefix)[:MAX_RECIPE_CLAUSES]:
        clause_calls, clause_findings = _check_clause(target, lineno, line_ok, clause.strip())
        calls += clause_calls
        findings.extend(clause_findings)
    return calls, findings


def _classify_header_line(stripped: str) -> tuple[str, str]:
    """Classify a non-recipe line inside the governance block."""
    if not stripped or stripped.startswith(("#", "-include ", "include ")):
        return "skip", ""
    if stripped.startswith(".PHONY:"):
        names = stripped.removeprefix(".PHONY:").split()[:MAX_CLAUSE_WORDS]
        return ("phony", "") if set(names) <= set(GOVERNANCE_TARGETS) else ("stop", "")
    if ":" in stripped and "=" not in stripped.split(":", 1)[0]:
        head = stripped.split(":", 1)[0].strip()
        return ("target", head) if head in GOVERNANCE_TARGETS else ("stop", "")
    return "skip", ""


def _parse_governance_block(
    lines: list[str], start_idx: int
) -> tuple[int | None, dict[str, list[tuple[int, str]]], list[str]]:
    """Collect (lineno, recipe_body) per governance target until the next section."""
    first_target_line: int | None = None
    current_target: str | None = None
    recipes: dict[str, list[tuple[int, str]]] = {}
    findings: list[str] = []
    for offset, raw in enumerate(lines[start_idx:MAX_MAKEFILE_LINES]):
        lineno = start_idx + offset + 1
        if raw.startswith("\t"):
            if current_target is None:
                findings.append(f"line {lineno}: recipe line outside a governance target")
            elif len(recipes[current_target]) < MAX_RECIPE_LINES:
                recipes[current_target].append((lineno, raw[1:]))
            continue
        kind, target = _classify_header_line(raw.strip())
        if kind == "stop":
            break
        if kind == "target":
            first_target_line = lineno if first_target_line is None else first_target_line
            current_target = target
            recipes.setdefault(target, [])
        elif kind == "phony":
            current_target = None
    return first_target_line, recipes, findings


def _check_target_recipes(recipes: dict[str, list[tuple[int, str]]]) -> list[str]:
    """Verify every required governance target exists and invokes $(PRAETORCTL)."""
    findings: list[str] = []
    for target in GOVERNANCE_TARGETS:
        if target not in recipes:
            findings.append(f"missing governance target {target!r}")
            continue
        target_calls = 0
        for lineno, body in recipes[target][:MAX_RECIPE_LINES]:
            calls, line_findings = _check_recipe_line(target, lineno, body)
            target_calls += calls
            findings.extend(line_findings)
        if target_calls == 0:
            findings.append(f"governance target {target!r} has no $(PRAETORCTL) engine call")
    return findings


def _find_markers(lines: list[str]) -> tuple[list[int], list[int]]:
    """Locate 1-based include line numbers and 0-based section header indices."""
    include_lines: list[int] = []
    header_indices: list[int] = []
    for idx, line in enumerate(lines[:MAX_MAKEFILE_LINES]):
        stripped = line.strip()
        if not line.startswith("\t") and stripped == INCLUDE_LINE:
            include_lines.append(idx + 1)
        elif stripped == SECTION_HEADER:
            header_indices.append(idx)
    return include_lines, header_indices


def check_governance_engine_pin(makefile_text: str) -> list[str]:
    """Return contract violations in Makefile text for the pinned Praetor engine."""
    lines = makefile_text.splitlines()[:MAX_MAKEFILE_LINES]
    include_lines, header_indices = _find_markers(lines)
    if len(header_indices) != 1:
        return [f"expected 1 {SECTION_HEADER!r} marker, found {len(header_indices)}"]
    first_target_line, recipes, findings = _parse_governance_block(lines, header_indices[0] + 1)
    if len(include_lines) != 1:
        findings.append(f"expected 1 {INCLUDE_LINE!r} line, found {len(include_lines)}")
    elif first_target_line is not None and include_lines[0] >= first_target_line:
        findings.append(
            f"{INCLUDE_LINE!r} at line {include_lines[0]} must precede target at line {first_target_line}"
        )
    findings.extend(_check_target_recipes(recipes))
    return findings


class MakeGovernanceEnginePinTest(unittest.TestCase):
    """Pin Makefile governance targets to .config/praetor/engine.mk and $(PRAETORCTL)."""

    makefile_text: ClassVar[str]

    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile_text = MAKEFILE.read_text(encoding="utf-8")

    def test_real_makefile_pins_governance_engine(self) -> None:
        """Positive: the real Makefile includes engine.mk once and calls $(PRAETORCTL)."""
        self.assertEqual(check_governance_engine_pin(self.makefile_text), [])

    def test_planted_standardsctl_recipe_fails(self) -> None:
        """Negative: a governance recipe calling @standardsctl fails."""
        mutated = self.makefile_text.replace(
            "\t@$(PRAETORCTL) audit\n", "\t@standardsctl audit\n", 1
        )
        self.assertTrue(mutated != self.makefile_text, "audit recipe anchor missing")
        findings = check_governance_engine_pin(mutated)
        self.assertTrue(any("standardsctl" in item for item in findings), findings)

    def test_planted_praetorctl_recipe_fails(self) -> None:
        """Negative: a governance recipe calling @praetorctl fails."""
        mutated = self.makefile_text.replace("\t@$(PRAETORCTL) audit\n", "\t@praetorctl audit\n", 1)
        self.assertTrue(mutated != self.makefile_text, "audit recipe anchor missing")
        findings = check_governance_engine_pin(mutated)
        self.assertTrue(any("praetorctl" in item for item in findings), findings)

    def test_planted_missing_include_fails(self) -> None:
        """Negative: removing -include .config/praetor/engine.mk fails."""
        mutated = self.makefile_text.replace(f"{INCLUDE_LINE}\n", "", 1)
        self.assertTrue(mutated != self.makefile_text, "include anchor missing")
        findings = check_governance_engine_pin(mutated)
        self.assertTrue(any(INCLUDE_LINE in item for item in findings), findings)

    def test_planted_include_after_governance_targets_fails(self) -> None:
        """Negative: placing -include after the governance targets fails."""
        without = self.makefile_text.replace(f"{INCLUDE_LINE}\n", "", 1)
        anchor = "\t@$(PRAETORCTL) dedupe scan .\n"
        mutated = without.replace(anchor, f"{anchor}{INCLUDE_LINE}\n", 1)
        self.assertTrue(mutated != self.makefile_text, "include/dedupe anchor missing")
        findings = check_governance_engine_pin(mutated)
        self.assertTrue(any("must precede" in item for item in findings), findings)

    def test_comment_line_with_praetorctl_does_not_count_as_call(self) -> None:
        """Boundary: $(PRAETORCTL) inside a comment does not satisfy a target call."""
        tab_comment = self.makefile_text.replace(
            "\t@$(PRAETORCTL) audit\n", "\t# $(PRAETORCTL) audit\n", 1
        )
        self.assertTrue(tab_comment != self.makefile_text, "audit recipe anchor missing")
        self.assertTrue(any("audit" in item for item in check_governance_engine_pin(tab_comment)))
        top_comment = self.makefile_text.replace(
            "\t@$(PRAETORCTL) audit\n", "# $(PRAETORCTL) audit\n", 1
        )
        self.assertTrue(any("audit" in item for item in check_governance_engine_pin(top_comment)))
        extra_comment = self.makefile_text.replace(
            "audit:\n", "# $(PRAETORCTL) replaces standardsctl and praetorctl\naudit:\n", 1
        )
        self.assertEqual(check_governance_engine_pin(extra_comment), [])

    def test_non_engine_words_and_paths_are_accepted(self) -> None:
        """Boundary: standardsctl-helper and tools/standardsctl.md are not literal engine calls."""
        mutated = self.makefile_text.replace(
            "\t@$(PRAETORCTL) audit\n",
            "\t@$(PRAETORCTL) audit tools/standardsctl.md\n"
            "\t@standardsctl-helper tools/standardsctl.md\n",
            1,
        )
        self.assertTrue(mutated != self.makefile_text, "audit recipe anchor missing")
        self.assertEqual(check_governance_engine_pin(mutated), [])

    def test_make_dedupe_subcall_line_is_accepted(self) -> None:
        """Boundary: the $(MAKE) --no-print-directory dedupe-check subcall is accepted."""
        self.assertIn("\t@$(MAKE) --no-print-directory dedupe-check\n", self.makefile_text)
        self.assertEqual(check_governance_engine_pin(self.makefile_text), [])

    def _find_posix_make(self, env: dict[str, str]) -> str:
        make_bin = shutil.which("make")
        if make_bin is None:
            reason = "make is not on PATH; the Linux Tooling Tests job runs the dry-run replay"
            print(f"make-governance-engine-pin: skipped replay: {reason}", file=sys.stderr)
            self.skipTest(reason)
        version = run_command(
            [make_bin, "--version"],
            allowed_executables=(make_bin,),
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=MAKE_TIMEOUT_SECONDS,
        )
        if re.search(r"windows32|mingw", f"{version.stdout}\n{version.stderr}", re.IGNORECASE):
            reason = (
                f"{make_bin} is a native Windows make; "
                "the Linux Tooling Tests job runs the dry-run replay"
            )
            print(f"make-governance-engine-pin: skipped replay: {reason}", file=sys.stderr)
            self.skipTest(reason)
        return make_bin

    @staticmethod
    def _split_dry_run_output(stdout: str) -> tuple[list[str], list[str]]:
        raw_lines = [
            line.strip() for line in stdout.splitlines()[:MAX_MAKEFILE_LINES] if line.strip()
        ]
        clauses: list[str] = []
        for line in raw_lines[:MAX_MAKEFILE_LINES]:
            for part in line.split("&&")[:MAX_RECIPE_CLAUSES]:
                stripped = part.strip()
                if stripped:
                    clauses.append(stripped)
        return raw_lines, clauses

    def test_verify_all_dry_run_uses_pinned_engine(self) -> None:
        """Replay: make -n verify-all expands PRAETOR_ENGINE across all four governance calls."""
        scrubbed = {"MAKEFLAGS", "MFLAGS", "MAKELEVEL", "PRAETORCTL", "PRAETOR_ENGINE"}
        env = {key: value for key, value in os.environ.items() if key not in scrubbed}
        make_bin = self._find_posix_make(env)
        pinned = "/nonexistent/pinned-engine"
        result = run_command(
            [
                make_bin,
                "-n",
                "--no-print-directory",
                "-C",
                str(REPO_ROOT),
                f"PRAETOR_ENGINE={pinned}",
                "verify-all",
            ],
            allowed_executables=(make_bin,),
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=MAKE_TIMEOUT_SECONDS,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        raw_lines, clauses = self._split_dry_run_output(result.stdout)
        for item in [*raw_lines, *clauses]:
            self.assertFalse(
                re.match(r"^(?:standardsctl|praetorctl)(?:\s|$)", item),
                f"unexpected literal engine invocation: {item!r}",
            )
        pinned_calls = [clause for clause in clauses if clause.startswith(f"{pinned} ")]
        self.assertEqual(
            pinned_calls,
            [
                f"{pinned} audit",
                f"{pinned} compile-context --verify",
                f"{pinned} hiss coverage --verify",
                f"{pinned} dedupe scan .",
            ],
        )


if __name__ == "__main__":
    unittest.main()
