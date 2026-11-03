#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Planted-defect recall for the float-identity Semgrep rule (ADR-1502).

`vmaf-test-float-identity-via-float-bits` in `.semgrep.yml` refuses `==` and
`!=` between two computed floating-point values in `core/test/`. CodeQL's
`cpp/equality-on-floats` reports the same shape (alerts 1593-1598 on
`test_read_pictures_convert.c`), but only in the sources its build compiles;
39 such comparisons in the CUDA, HIP and SYCL twin tests never reached it.

The test copies the real rule file into a throwaway project and plants:

- computed `double` and `float` comparisons in `core/test/`, arithmetic on
  one side or both, which the rule must report;
- the comparisons CodeQL allows (a decimal or hexadecimal literal, a macro or
  k-constant, the `x != x` NaN idiom, integers, calls that return integers),
  which it must not report;
- the same computed comparisons outside `core/test/`, which are out of the
  rule's scope.

The last test scans the real `core/test/` tree and requires zero findings. A
region Semgrep cannot parse is invisible to the rule, so the scan also fails on
every parse diagnostic outside the files listed in
`.config/lint-exceptions.d/vmaf-test-float-identity-parse.toml` (repair work:
issue 3252), on an expired entry, and on a listed file that now parses.
Semgrep is located as in `test_semgrep_vendored_scope.py`.
"""

from __future__ import annotations

import datetime as dt
import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import lint_exceptions
from scripts.ci.tests import test_semgrep_vendored_scope as vendored_scope
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
RULE = "vmaf-test-float-identity-via-float-bits"
IN_SCOPE = "core/test/planted_float_identity.c"
IN_SCOPE_CPP = "core/test/planted_float_identity.cpp"
OUT_OF_SCOPE = "core/src/planted_float_identity.c"
PLANTED = """\
#include <stdbool.h>

#define LIMIT 2.0
static const double kLimit = 3.0;
static double computed(void);
static unsigned bits(double value);

bool planted(double x, float p, float q, int i, int k, double u, double v,
             long long ll, short s, unsigned char c)
{
    double y = computed();
    double d = computed();
    double a = computed();
    double b = computed();
    bool hits = x != y;          /* reported: computed double != */
    hits = hits || x == y;       /* reported: computed double == */
    hits = hits || p == q;       /* reported: computed float == */
    hits = hits || p != q;       /* reported: computed float != */
    hits = hits || 1.0 + y == x;  /* reported: literal prefix, computed */
    hits = hits || kLimit + y != x; /* reported: k-constant prefix, computed */
    hits = hits || (double)(y * 2.0) == x; /* reported: cast of a computed value */
    hits = hits || -LIMIT * y == x; /* reported: macro prefix, computed */
    hits = hits || (x + y) == (u + v); /* reported: arithmetic both sides */
    hits = hits || (p * q) != (q - p); /* reported: float arithmetic both sides */
    hits = hits || computed() != computed(); /* reported: two call results != */
    hits = hits || 0x10 + y == x; /* reported: hex literal prefix, computed */
    hits = hits || (double)x == y; /* reported: (double)x == y */
    hits = hits || d == (int)y;  /* reported: double compared to integer cast */
    bool quiet = x == 0.0;       /* literal */
    quiet = quiet || 1.5 != y;   /* literal on the left */
    quiet = quiet || x == LIMIT; /* macro */
    quiet = quiet || y == kLimit; /* k-constant */
    quiet = quiet || x != x;     /* NaN idiom */
    quiet = quiet || (x) != (x); /* NaN idiom grouping parens */
    quiet = quiet || i == k;     /* integers */
    quiet = quiet || x == -1.5e3; /* signed literal with exponent */
    quiet = quiet || p == .5f;   /* float literal */
    quiet = quiet || y != -kLimit; /* signed k-constant */
    quiet = quiet || i == (int)y; /* integer cast of a double */
    quiet = quiet || (int)a == (int)b; /* integer cast on both sides */
    quiet = quiet || x == 0x1p0; /* hexadecimal floating literal */
    quiet = quiet || p != -0x1.8p-3f; /* signed hexadecimal float literal */
    quiet = quiet || (i + (int)x) == k; /* integer arithmetic */
    quiet = quiet || bits(x + y) == bits(u + v); /* calls return integers */
    quiet = quiet || x == 0x10;  /* hexadecimal integer literal */
    quiet = quiet || y != 1U;    /* suffixed integer literal */
    quiet = quiet || x == 0x10u; /* hex suffixed integer literal */
    quiet = quiet || y == 3ULL;  /* suffixed integer literal */
    quiet = quiet || ll == (long long)y; /* long long cast */
    quiet = quiet || s == (short)x; /* short cast */
    quiet = quiet || c == (unsigned char)x; /* unsigned char cast */
    quiet = quiet || i == (unsigned long long)y; /* unsigned long long cast */
    return hits || quiet;
}

bool deref_planted(double *p)
{
    return *p != *p; /* reported: dereference NaN self-check */
}
"""
PLANTED_HITS = tuple(n for n, line in enumerate(PLANTED.splitlines(), 1) if "/* reported" in line)
PLANTED_CPP = """\
bool planted_cpp(float f)
{
    bool quiet = f == 0b1010;
    quiet = quiet || f == 1'000;
    return quiet;
}
"""
PLANTED_CONST_NAMES = """\
double kScale = computed();
float GAIN;
double kNoted = computed(); // const
float LIMIT; /* const */
static const double kOk = 1.0;
double const kEast = 1.0;
double kTail = 1.0; const int n = 2;
/* const */ double kLead = computed();
printf("https://example"); double kResult = computed();
puts("double kInString = 1.0;");
double
kMulti = computed();
/* const
*/ double kBlock = computed();
static const double
kSplitOk = 1.0;
void f(const char *label,
       double kParam, double actual);
void g(double x, const double kParamOk, int n);
void h(int n, double kLast);
double ordinary, kComputed = computed();
const double a, kB = 1.0;
double scaled(UINT32 n);
double X = computed();
double kA = 1.0, *const kP = 0;
double kB = (const double)1.0;
auto kC = 1.0;
double &kRef = x;
const double kD = 1.0, kE = 2.0;
"""
FLOAT_TYPE = re.compile(r"\b(?:double|float|Scores|MotionScores|auto)\b")
CONSTANT_NAME = re.compile(r"k[A-Z]\w*|[A-Z][A-Z0-9_]*")
DECLARED_NAME = re.compile(r"[\s*&]*(\w+)\b(?!\s*\()")
C_LEXEME = re.compile(
    r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?(?:\*/|\Z)',
    re.DOTALL,
)
CONST_QUALIFIER = re.compile(r"\b(?:const|constexpr)\b")
PTR_CONST = re.compile(r"\*\s*const\b")
FLOAT_INIT = re.compile(
    r"\b[0-9]*\.[0-9]+|\b[0-9]+\.[0-9]*|\b[0-9]+[fF]\b|\b[0-9]+[eE][+-]?[0-9]+|\b(?:computed|sqrt|sin|cos|fabs)\b"
)
EXCEPTED = "core/test/test_pelorus_interop.c"  # pelorus mirror, declared exception
PARSE_RULE = "vmaf-test-float-identity-parse"  # files with a known parse diagnostic
UNPARSABLE = "core/test/planted_unparsable.c"
UNPARSABLE_TEXT = "int broken(void) { return ((1 + ; }\n"
# The C and C++ suffixes Semgrep reads as c / cpp; the pre-commit selector of
# the hook names the same set (test_hook_selects_every_source_suffix).
SOURCE_SUFFIXES = frozenset({".c", ".h", ".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx"})


def rule_excludes() -> set[str]:
    """The repository paths the production rule's paths.exclude names."""
    rules = yaml.safe_load((ROOT / ".semgrep.yml").read_text(encoding="utf-8"))["rules"]
    (rule,) = [r for r in rules if r["id"] == RULE]
    return {path.removeprefix("/") for path in rule.get("paths", {}).get("exclude", [])}


def live_exceptions(day: dt.date, rule: str = RULE) -> set[str]:
    """The files the declared exception list excepts from `rule` on `day`."""
    entries, findings = lint_exceptions.load(ROOT)
    assert not findings, findings
    return {e.path for e in entries if e.rule == rule and e.expires >= day}


def diagnostic_problems(diagnosed: set[str], listed: set[str]) -> list[str]:
    """Each parse diagnostic outside `listed`, and each listed file that now parses."""
    return [
        f"{path}: Semgrep parse diagnostic, not listed" for path in sorted(diagnosed - listed)
    ] + [
        f"{path}: listed as unparsable but parses; drop its entry"
        for path in sorted(listed - diagnosed)
    ]


def core_test_sources(root: Path) -> list[str]:
    """Sorted repo-relative POSIX paths of C and C++ sources under `core/test/`."""
    sources: list[str] = []
    for path in (root / "core/test").rglob("*"):
        if path.suffix in SOURCE_SUFFIXES:
            relative = path.relative_to(root)
            if relative.parts[:3] != ("core", "test", "data"):
                sources.append(relative.as_posix())
    return sorted(sources)


def _blank_lexeme(match: re.Match[str]) -> str:
    """A comment becomes spaces; a literal keeps its quotes, its body spaces."""
    lexeme = match.group(0)
    if lexeme[0] in "\"'":
        return lexeme[0] + re.sub(r"[^\n]", " ", lexeme[1:-1]) + lexeme[-1]
    return re.sub(r"[^\n]", " ", lexeme)


def declaration_start(code: str, pos: int) -> int:
    """Offset where the declaration holding `pos` starts."""
    stmt = max(code.rfind(sep, 0, pos) for sep in ";{}") + 1
    opens: list[int] = []
    for offset, char in enumerate(code[stmt:pos], start=stmt):
        if char == "(":
            opens.append(offset + 1)
        elif char == ")" and opens:
            opens.pop()
        elif char == "," and opens:
            opens[-1] = offset + 1
    return opens[-1] if opens else stmt


def declarators(code: str, start: int, in_params: bool) -> list[tuple[int, int]]:
    """Spans of the declarators that follow a type keyword at `start`."""
    spans: list[tuple[int, int]] = []
    depth = 0
    begin = start
    for offset in range(start, len(code)):
        char = code[offset]
        if char in "([":
            depth += 1
        elif char in ")]" and depth > 0:
            depth -= 1
        elif depth == 0:
            if char in ");{}" or (char == "," and in_params):
                break
            if char == ",":
                spans.append((begin, offset))
                begin = offset + 1
    else:
        offset = len(code)
    spans.append((begin, offset))
    return spans


def constant_name_findings(text: str, name: str) -> list[str]:
    """Declarations of a non-const float or double with a constant-style name."""
    code = C_LEXEME.sub(_blank_lexeme, text)
    lines = text.splitlines()
    found: list[str] = []
    seen: set[int] = set()
    for kind in FLOAT_TYPE.finditer(code):
        start = declaration_start(code, kind.start())
        if start in seen:
            continue
        seen.add(start)
        in_params = start > 0 and code[start - 1] in "(,"
        after_kind = kind.end()
        m_qual = re.match(r"\s*(?:const|constexpr)\b", code[after_kind:])
        first_decl_start = after_kind + m_qual.end() if m_qual else after_kind
        if CONST_QUALIFIER.search(code[start:first_decl_start]):
            continue
        spans = declarators(code, first_decl_start, in_params)
        is_auto = kind.group(0) == "auto"
        for begin, end in spans:
            decl_text = code[begin:end]
            eq_pos = -1
            depth = 0
            for i, c in enumerate(decl_text):
                if c in "([":
                    depth += 1
                elif c in ")]" and depth > 0:
                    depth -= 1
                elif depth == 0 and c == "=":
                    eq_pos = i
                    break
            decl_head = decl_text[:eq_pos] if eq_pos != -1 else decl_text
            init_text = decl_text[eq_pos + 1 :] if eq_pos != -1 else ""
            if is_auto and not FLOAT_INIT.search(init_text):
                continue
            if PTR_CONST.search(decl_head):
                continue
            decl = DECLARED_NAME.match(decl_head)
            if decl and CONSTANT_NAME.fullmatch(decl.group(1)):
                n = code.count("\n", 0, kind.start()) + 1
                found.append(f"{name}:{n}: {lines[n - 1].strip()}")
                break
    return found


class SemgrepFloatIdentity(unittest.TestCase):
    def setUp(self) -> None:
        # One Semgrep launcher: the vendored-scope case finds semgrep on PATH
        # or the pinned image, and fails when neither is available.
        self.launcher = vendored_scope.SemgrepVendoredScope("setUp")
        self.launcher.setUp()

    def scan(self, project: Path, targets: list[str]) -> dict[str, list[int]]:
        """Return the rule's finding lines per path, from the production rule file."""
        return self.scan_with_diagnostics(project, targets)[0]

    def scan_with_diagnostics(
        self, project: Path, targets: list[str]
    ) -> tuple[dict[str, list[int]], set[str]]:
        """Return the rule's finding lines per path and the paths Semgrep could not parse."""
        prefix, executable = self.launcher.semgrep_argv(project)
        argv = [
            *prefix,
            "scan",
            "--config=.semgrep.yml",
            "--json",
            "--quiet",
            "--metrics=off",
            "--disable-version-check",
            "--jobs",
            "1",
            "--no-git-ignore",
            *targets,
        ]
        env = dict(os.environ)
        env_dir = Path(tempfile.gettempdir()) / "vmafx-semgrep-env"
        env_dir.mkdir(parents=True, exist_ok=True)
        env.setdefault("XDG_CONFIG_HOME", str(env_dir))
        env.setdefault("SEMGREP_SETTINGS_FILE", str(env_dir / "semgrep-settings.yaml"))
        env.setdefault("SEMGREP_LOG_FILE", str(env_dir / "semgrep.log"))
        proc = run_command(
            argv,
            allowed_executables=(executable,),
            cwd=project,
            env=env,
            capture_output=True,
            text=True,
            timeout_seconds=300,
            max_output_bytes=16 * 1_048_576,
        )
        self.assertIn(proc.returncode, (0, 1), proc.stderr)
        report = json.loads(proc.stdout)
        diagnosed = set()
        for error in report["errors"]:
            self.assertIn("path", error, f"Semgrep error without a path: {error}")
            diagnosed.add(Path(error["path"]).as_posix())
        lines: dict[str, list[int]] = {}
        for result in report["results"]:
            if result["check_id"].rsplit(".", 1)[-1] == RULE:
                path = Path(result["path"]).as_posix()
                lines.setdefault(path, []).append(result["start"]["line"])
        return {path: sorted(found) for path, found in lines.items()}, diagnosed

    def planted_project(self) -> Path:
        temp = tempfile.TemporaryDirectory(prefix="vmafx-semgrep-float-")
        self.addCleanup(temp.cleanup)
        project = Path(temp.name)
        shutil.copyfile(ROOT / ".semgrep.yml", project / ".semgrep.yml")
        for relative in (IN_SCOPE, OUT_OF_SCOPE):
            target = project / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(PLANTED, encoding="utf-8")
        target_cpp = project / IN_SCOPE_CPP
        target_cpp.parent.mkdir(parents=True, exist_ok=True)
        target_cpp.write_text(PLANTED_CPP, encoding="utf-8")
        return project

    def test_reports_computed_comparisons_only(self) -> None:
        lines = self.scan(self.planted_project(), [IN_SCOPE, OUT_OF_SCOPE, IN_SCOPE_CPP])
        self.assertEqual(
            lines.get(IN_SCOPE),
            list(PLANTED_HITS),
            "the rule must report the ten computed comparisons and none of the "
            "literal, macro, k-constant, NaN-idiom, integer or call comparisons",
        )
        self.assertNotIn(
            IN_SCOPE_CPP,
            lines,
            "C++ binary literal and digit separator must not be reported",
        )

    def test_scope_is_core_test(self) -> None:
        lines = self.scan(self.planted_project(), [OUT_OF_SCOPE])
        self.assertNotIn(OUT_OF_SCOPE, lines, "the rule reached outside core/test/")

    def test_excludes_are_the_declared_exceptions(self) -> None:
        self.assertEqual(
            rule_excludes(),
            live_exceptions(lint_exceptions.today()),
            "paths.exclude of the rule must name exactly the live entries of "
            f".config/lint-exceptions.d/{RULE}.toml; an expired entry leaves the rule",
        )

    def test_an_expired_exception_no_longer_matches(self) -> None:
        self.assertIn(EXCEPTED, rule_excludes())
        expiry = max(e.expires for e in lint_exceptions.load(ROOT)[0] if e.rule == RULE)
        self.assertNotEqual(rule_excludes(), live_exceptions(expiry + dt.timedelta(days=1)))

    def test_exclusion_covers_the_excepted_file_only(self) -> None:
        project = self.planted_project()
        sibling = "core/test/planted_beside_mirror.c"
        for relative in (EXCEPTED, sibling):
            (project / relative).write_text(PLANTED, encoding="utf-8")
        lines = self.scan(project, [EXCEPTED, sibling])
        self.assertNotIn(EXCEPTED, lines, "the declared exception was not applied")
        self.assertEqual(lines.get(sibling), list(PLANTED_HITS))

    def test_unparsable_file_is_a_diagnostic(self) -> None:
        project = self.planted_project()
        (project / UNPARSABLE).write_text(UNPARSABLE_TEXT, encoding="utf-8")
        _, diagnosed = self.scan_with_diagnostics(project, [UNPARSABLE, IN_SCOPE])
        self.assertEqual(diagnosed, {UNPARSABLE}, "the planted parse error went unreported")
        # A list naming a file that parses, and missing the one that does not.
        self.assertEqual(
            diagnostic_problems(diagnosed, {IN_SCOPE}),
            [
                f"{UNPARSABLE}: Semgrep parse diagnostic, not listed",
                f"{IN_SCOPE}: listed as unparsable but parses; drop its entry",
            ],
        )

    def test_parse_entries_expire(self) -> None:
        entries = [e for e in lint_exceptions.load(ROOT)[0] if e.rule == PARSE_RULE]
        self.assertTrue(entries)
        self.assertEqual(
            live_exceptions(max(e.expires for e in entries) + dt.timedelta(days=1), PARSE_RULE),
            set(),
        )

    def test_hook_selects_every_source_suffix(self) -> None:
        hooks = [
            hook
            for repo in yaml.safe_load((ROOT / ".pre-commit-config.yaml").read_text("utf-8"))[
                "repos"
            ]
            for hook in repo["hooks"]
            if "test_semgrep_float_identity.py" in str(hook.get("entry", ""))
        ]
        self.assertEqual(len(hooks), 1, "expected one float-identity pre-commit hook")
        selector: re.Pattern[str] = re.compile(str(hooks[0]["files"]))
        for suffix in sorted(SOURCE_SUFFIXES):
            self.assertRegex(f"core/test/x{suffix}", selector, f"hook skips {suffix} tests")

    def test_sources_ignore_checkout_path_named_data(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "data" / "repo"
            (root / "core/test/data").mkdir(parents=True)
            (root / "core/test/a.c").write_text("", encoding="utf-8")
            (root / "core/test/data/b.c").write_text("", encoding="utf-8")
            self.assertEqual(core_test_sources(root), ["core/test/a.c"])

    def test_real_tests_compare_floats_through_float_bits(self) -> None:
        targets = core_test_sources(ROOT)
        self.assertTrue(targets, "core/test has no C or C++ sources")
        lines, diagnosed = self.scan_with_diagnostics(ROOT, targets)
        listed = live_exceptions(lint_exceptions.today(), PARSE_RULE)
        self.assertEqual(
            diagnostic_problems(diagnosed, listed),
            [],
            "Semgrep could not parse a region the float-identity rule must read; repair the "
            "file (issue 3252) or, for a measured existing file, list it in "
            f".config/lint-exceptions.d/{PARSE_RULE}.toml",
        )
        self.assertEqual(
            lines,
            {},
            "a test compares two computed floating-point values with == or !=; use "
            "vmaf_test_identical_f64() / _f32() from core/test/float_bits.h",
        )

    def test_constant_name_declarations_are_const(self) -> None:
        got = constant_name_findings(PLANTED_CONST_NAMES, "planted.c")
        want = [
            f"planted.c:{n}:" for n in (1, 2, 3, 4, 7, 8, 9, 11, 14, 18, 20, 21, 24, 25, 26, 27, 28)
        ]
        self.assertEqual([g.split(" ", 1)[0] for g in got], want)
        found: list[str] = []
        for target in core_test_sources(ROOT):
            text = (ROOT / target).read_text(encoding="utf-8", errors="replace")
            found.extend(constant_name_findings(text, target))
        self.assertEqual(
            found,
            [],
            "non-const float with a constant-style name found in core/test; make it "
            "const or rename it so the macro/k-constant exemption remains sound",
        )


if __name__ == "__main__":
    unittest.main()
