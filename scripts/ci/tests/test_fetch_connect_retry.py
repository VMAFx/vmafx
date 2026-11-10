#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Audit curl/wget fetches and tidy-lane APT offline; git/other APT gap: #3392."""

from __future__ import annotations

import re
import shlex
import shutil
import sys
import tempfile
import unittest
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

# Curl/wget fetches + tidy-lane APT only; git clone/other APT gap: #3392.
# Exact path + parsed URL + reason; exempt commands skip every check.
ALLOWLIST = (
    (
        "scripts/ci/install-rocm-from-image.sh",
        "https://${REGISTRY}/v2/${REPO}/blobs/${digest}",
        "extract_layer retries the whole curl | decompress | tar pipeline LAYER_ATTEMPTS times; "
        "an inner retry would append to the pipe",
    ),
    (
        "scripts/ci/observability-compose-smoke.sh",
        "http://localhost:9090/api/v1/query?query=up%7Bjob%3D%22vmafx-node%22%7D%3D%3D1",
        "node_scraped runs inside a 90-attempt outer poll; final assertion must fail fast",
    ),
    (
        "scripts/release/smoke-published-image.sh",
        "http://$address$path",
        "wait_ready repeats all endpoints at most READY_TRIES times",
    ),
    (
        ".github/workflows/docker-publish-production.yml",
        "http://127.0.0.1:18080/healthz",
        "health check already has a 30-attempt outer poll",
    ),
    (
        ".github/workflows/docker-publish-production.yml",
        "http://127.0.0.1:18080/readyz",
        "single readiness assertion after bounded health poll; failure must stop the smoke",
    ),
    (
        "scripts/git-hooks/pre-push-pr-body-lint.sh",
        "https://github.com/${repository}/pulls",
        "bounded fallback lookup returns 2 to its caller, which handles lookup failure",
    ),
    (
        "scripts/git-hooks/pre-push-pr-body-lint.sh",
        "https://github.com/${repository}/pull/${pull_number}",
        "bounded fallback page lookup returns 2 to its caller, which handles lookup failure",
    ),
    (
        "test/e2e/score-smoke.sh",
        "http://127.0.0.1:${LOCAL_PORT}/readyz",
        "readiness check already has a 60-attempt outer poll",
    ),
    (
        "test/e2e/score-smoke.sh",
        "http://127.0.0.1:${LOCAL_PORT}/v1/score",
        "single non-idempotent scoring POST through the local port-forward; fail fast",
    ),
)
FETCH = re.compile(r"(?<![\w.-])(curl(?:\.exe)?|wget)\s+(?=[\"']?(?:-|https?://|\$|%))")
SHELL_HEADER = re.compile(r"^#![^\n]*\b(?:sh|ash|bash|dash|ksh|zsh|fish|pwsh)\b")
SHELL_SUFFIXES = frozenset((".sh", ".bash", ".ksh", ".zsh", ".fish", ".ps1", ".cmd", ".bat"))
CONTROL = frozenset((";", "&&", "||", "|", "&", "(", ")", ";;"))


def in_scope(path: str, data: str) -> bool:
    """Shell extensions/shebangs, Actions workflows, and container recipe names."""
    name = Path(path)
    return (
        name.suffix in SHELL_SUFFIXES
        or bool(SHELL_HEADER.match(data))
        or (path.startswith(".github/workflows/") and name.suffix in (".yml", ".yaml"))
        or bool(re.search(r"(?:^|\.)(?:Dockerfile|Containerfile)(?:\.|$)", name.name))
    )


def tracked_sources() -> list[tuple[str, str]]:
    """Read Git's tracked files only; never launch a fetch or execute a scanned script."""
    git = shutil.which("git")
    if git is None:
        raise AssertionError("git is required to enumerate tracked fetch sites")
    result = run_command(
        [git, "ls-files", "-z"],
        allowed_executables=[git],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=30,
    )
    sources: list[tuple[str, str]] = []
    for path in result.stdout.split("\0"):
        source = ROOT / path
        if not path or not source.is_file() or source.is_symlink():
            continue
        # A shebang can make an extensionless file a shell script. Decode with
        # replacement so binary assets need no special extension allow-list.
        with source.open("rb") as handle:
            header = handle.read(128)
        if in_scope(path, header.decode("utf-8", errors="replace")):
            sources.append((path, source.read_text(encoding="utf-8")))
    return sources


def logical_lines(text: str) -> list[tuple[int, str]]:
    """Join shell, PowerShell, and cmd continuations without losing source locations."""
    lines: list[tuple[int, str]] = []
    pending = ""
    start = 1
    for number, line in enumerate(text.splitlines(), 1):
        if not pending:
            start = number
        if line.endswith(("\\", "`", "^")):
            pending += line[:-1] + " "
        else:
            lines.append((start, pending + line))
            pending = ""
    if pending:
        lines.append((start, pending))
    return lines


# curl 8.23.0, commit 3163ec065643ba20709f190111f42c3e1fe95273:
# src/tool_getparam.c aliases[]; src/tool_getparam.h:321-326.
# ARG_STRG/FILE/SECS/UNUM consume values; ARG_BOOL accepts --no-.
# Rows: long name, short letter, takes value, accepts --no-.
CURL_OPTIONS = (
    ("connect-timeout", None, True, False),
    ("create-dirs", None, False, True),
    ("data", "d", True, False),
    ("data-urlencode", None, True, False),
    ("fail", "f", False, True),
    ("fail-with-body", None, False, True),
    ("get", "G", False, True),
    ("header", "H", True, False),
    ("location", "L", False, True),
    ("max-time", "m", True, False),
    ("oauth2-bearer", None, True, False),
    ("output", "o", True, False),
    ("proto", None, True, False),
    ("remove-on-error", None, False, True),
    ("retry", None, True, False),
    ("retry-all-errors", None, False, True),
    ("retry-connrefused", None, False, True),
    ("retry-delay", None, True, False),
    ("retry-max-time", None, True, False),
    ("show-error", "S", False, True),
    ("silent", "s", False, True),
    ("tlsv1.2", None, False, False),
    ("url", None, True, False),
    ("user-agent", "A", True, False),
    ("write-out", "w", True, False),
    ("head", "I", False, True),
    ("remote-name", "O", False, True),
)
# GNU Wget 1.25.0, LC_ALL=C wget --help; flag --no- forms measured at rc 0
# in review; --no-tries=3 rejected at rc 2. Same row format as curl.
WGET_OPTIONS = (
    ("output-document", "O", True, False),
    ("quiet", "q", False, True),
    ("retry-connrefused", None, False, True),
    ("retry-on-host-error", None, False, True),
    ("timeout", "T", True, False),
    ("tries", "t", True, False),
    ("waitretry", None, True, False),
    ("spider", None, False, True),
)
APT_RETRY_PATHS = ("scripts/dev/tidy-lane.sh",)


@dataclass
class ParsedFetch:
    """Hold consumed options, URLs, redirection and parser diagnostics."""

    tool: str
    values: dict[str, str] = field(default_factory=dict)
    flags: dict[str, bool] = field(default_factory=dict)
    urls: list[str] = field(default_factory=list)
    stdout_target: str = ""
    errors: list[str] = field(default_factory=list)


def quoted_end(command: str, index: int) -> int:
    """Skip a quoted span while honoring double-quote backslash escapes."""
    quote = command[index]
    index += 1
    while index < len(command):
        char = command[index]
        index += 1
        if quote == '"' and char == "\\":
            index += 1
        elif char == quote:
            break
    return index


def strip_comment(command: str) -> str:
    """Strip shell comments while retaining quoted and in-word hash characters."""
    index = 0
    while index < len(command):
        char = command[index]
        if char == "\\":
            index += 2
            continue
        if char in ("'", '"'):
            index = quoted_end(command, index)
            continue
        word_start = index == 0 or command[index - 1].isspace() or command[index - 1] in ";&|()"
        if char == "#" and word_start:
            return command[:index]
        index += 1
    return command


def command_args(command: str) -> list[str]:
    """Tokenize one fetch up to shell control after removing actual comments."""
    command = strip_comment(command)
    lexer = shlex.shlex(command, posix=True, punctuation_chars=";&|()")
    lexer.whitespace_split = True
    lexer.commenters = ""
    try:
        arguments = list(lexer)
    except ValueError:
        # Embedded shell/YAML fragments may end inside an enclosing quote.
        arguments = [argument.strip("\"'") for argument in command.split()]
    args: list[str] = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "&" and index + 1 < len(arguments) and arguments[index + 1].startswith(">"):
            index += 1
            argument += arguments[index]
        elif argument in CONTROL:
            break
        args.append(argument)
        index += 1
    return args


def consume_value(
    args: list[str], index: int, attached: str | None, parsed: ParsedFetch, name: str
) -> int:
    """Consume an attached or following value without interpreting its contents."""
    if attached is not None:
        value = attached
        consumed = 1
    elif index + 1 < len(args):
        value = args[index + 1]
        consumed = 2
    else:
        parsed.errors.append(f"{parsed.tool} option --{name} needs a value")
        return 1
    parsed.values[name] = value
    if name == "url":
        parsed.urls.append(value)
    return consumed


def parse_long(args: list[str], index: int, parsed: ParsedFetch) -> int:
    """Validate a long option and consume its value or final flag state."""
    option, equals, value = args[index][2:].partition("=")
    negated = option.startswith("no-")
    name = option[3:] if negated else option
    rows = WGET_OPTIONS if parsed.tool == "wget" else CURL_OPTIONS
    row = next((row for row in rows if row[0] == name), None)
    if row is None or (negated and not row[3]):
        parsed.errors.append(f"unknown {parsed.tool} option --{option}")
        return 1
    if row[2]:
        return consume_value(args, index, value if equals else None, parsed, name)
    if equals:
        parsed.errors.append(f"{parsed.tool} flag --{option} does not take a value")
    else:
        parsed.flags[name] = not negated
    return 1


def parse_short(args: list[str], index: int, parsed: ParsedFetch) -> int:
    """Read short bundles left to right with value options consuming the remainder."""
    rows = WGET_OPTIONS if parsed.tool == "wget" else CURL_OPTIONS
    bundle = args[index][1:]
    for offset, letter in enumerate(bundle):
        row = next((row for row in rows if row[1] == letter), None)
        if row is None:
            parsed.errors.append(f"unknown {parsed.tool} option -{letter}")
            continue
        if row[2]:
            attached = bundle[offset + 1 :] or None
            return consume_value(args, index, attached, parsed, row[0])
        parsed.flags[row[0]] = True
    return 1


def parse_redirect(args: list[str], index: int, parsed: ParsedFetch) -> int:
    """Skip redirection targets and record the destination of standard output."""
    match = re.match(r"([012&]?)(>>?|<<?)(.*)", args[index])
    if match is None:
        return 0
    descriptor, operator, target = match.groups()
    consumed = 1
    if not target and index + 1 < len(args):
        target = args[index + 1]
        consumed = 2
    if operator.startswith(">") and descriptor != "2":
        parsed.stdout_target = target
    return consumed


def parse_fetch(args: list[str]) -> ParsedFetch:
    """Parse only allow-listed curl or wget options with their real value consumption."""
    parsed = ParsedFetch("curl" if args[0] == "curl.exe" else args[0])
    index = 1
    positional = False
    while index < len(args):
        arg = args[index]
        redirected = parse_redirect(args, index, parsed)
        if redirected:
            index += redirected
            continue
        if arg == "--":
            positional = True
        elif not positional and arg.startswith("--"):
            index += parse_long(args, index, parsed)
            continue
        elif not positional and arg.startswith("-") and arg != "-":
            index += parse_short(args, index, parsed)
            continue
        else:
            parsed.urls.append(arg)
        index += 1
    return parsed


def fetch_commands(text: str) -> list[tuple[int, ParsedFetch]]:
    """Inventory parsed literal and variable URLs, including long URL options."""
    commands: list[tuple[int, ParsedFetch]] = []
    for line_number, source_line in logical_lines(text):
        line = strip_comment(source_line)
        for match in FETCH.finditer(line):
            parsed = parse_fetch(command_args(line[match.start() :]))
            if any(re.match(r"https?://|\$|%", url) for url in parsed.urls):
                commands.append((line_number, parsed))
    return commands


def positive_count(parsed: ParsedFetch, option: str) -> bool:
    """Accept only literal positive integer retry counts from consumed values."""
    return bool(re.fullmatch(r"[1-9][0-9]*", parsed.values.get(option, "")))


def retry_errors(parsed: ParsedFetch) -> list[str]:
    """Require positive retries and the fetch tool's connection error switches."""
    if parsed.tool == "wget":
        if (
            positive_count(parsed, "tries")
            and parsed.flags.get("retry-connrefused")
            and parsed.flags.get("retry-on-host-error")
        ):
            return []
        return ["wget needs positive --tries, --retry-connrefused and --retry-on-host-error"]
    errors: list[str] = []
    if not positive_count(parsed, "retry"):
        errors.append("curl needs positive --retry")
    if not parsed.flags.get("retry-all-errors"):
        errors.append("curl needs --retry-all-errors")
    return errors


def body_errors(parsed: ParsedFetch) -> list[str]:
    """Reject retried bodies on stdout except explicit disposal to /dev/null."""
    if parsed.tool == "wget":
        stdout = parsed.values.get("output-document") == "-" and not parsed.flags.get("spider")
    else:
        stdout = (
            parsed.values.get("output", "-") == "-"
            and not parsed.flags.get("remote-name")
            and not parsed.flags.get("head")
        )
    if stdout and parsed.stdout_target != "/dev/null":
        return [f"{parsed.tool} retry must write the body to a file, not stdout"]
    return []


def matching_exemptions(
    path: str, parsed: ParsedFetch, allowlist: tuple[tuple[str, str, str], ...]
) -> list[tuple[str, str, str]]:
    """Match exemptions by both tracked path and complete parsed URL."""
    return [entry for entry in allowlist if entry[0] == path and entry[1] in parsed.urls]


def apt_errors(path: str, text: str) -> list[str]:
    """Require positive literal APT retry counts in the declared tidy lane only."""
    errors: list[str] = []
    if path not in APT_RETRY_PATHS:
        return errors
    for line, command in logical_lines(text):
        command_text = strip_comment(command)
        for match in re.finditer(r"(?<![\w.-])apt-get\s+", command_text):
            args = command_args(command_text[match.end() :])
            if {"update", "install"} & set(args) and not apt_positive_retries(args):
                errors.append(f"{path}:{line}: apt-get needs -o Acquire::Retries=N with N positive")
    return errors


def apt_positive_retries(args: list[str]) -> bool:
    """Validate the final explicit APT retry setting against positive literal integers."""
    retries = [
        args[index + 1]
        for index, arg in enumerate(args[:-1])
        if arg == "-o" and args[index + 1].startswith("Acquire::Retries=")
    ]
    return bool(retries and re.fullmatch(r"Acquire::Retries=[1-9][0-9]*", retries[-1]))


def audit_sources(
    sources: list[tuple[str, str]], allowlist: tuple[tuple[str, str, str], ...] = ALLOWLIST
) -> list[str]:
    """Audit parsed fetches and APT while rejecting stale or ambiguous exemptions."""
    errors: list[str] = []
    seen: Counter[tuple[str, str, str]] = Counter()
    for path, text in sources:
        errors.extend(apt_errors(path, text))
        for line, parsed in fetch_commands(text):
            matches = matching_exemptions(path, parsed, allowlist)
            if matches:
                seen.update(matches)
            else:
                findings = parsed.errors + retry_errors(parsed) + body_errors(parsed)
                errors.extend(f"{path}:{line}: {error}" for error in findings)
    for entry in allowlist:
        if not entry[2] or seen[entry] != 1:
            errors.append(
                f"{entry[0]}: stale/ambiguous allow-list entry ({seen[entry]}): {entry[1]}"
            )
    return errors


class FetchConnectRetryTests(unittest.TestCase):
    def check_script(self, text: str) -> list[str]:
        """Audit a temporary shell source without executing its commands."""
        with tempfile.TemporaryDirectory() as temporary:
            script = Path(temporary) / "fetch.sh"
            script.write_text(text, encoding="utf-8")
            return audit_sources([(script.name, script.read_text(encoding="utf-8"))], ())

    def test_compliant_temporary_script(self) -> None:
        """Accept curl and wget with every required retry option."""
        self.assertEqual(
            self.check_script(
                "curl -o out -fsSL --retry 3 --retry-all-errors https://example.invalid/x\n"
                "wget -q --tries=3 --retry-connrefused --retry-on-host-error "
                "https://example.invalid/x\n"
            ),
            [],
        )

    def test_plain_retry_does_not_cover_refused_connection(self) -> None:
        """Reject curl retries that omit all-errors coverage."""
        self.assertTrue(
            self.check_script("curl -o out -fsSL --retry 3 https://example.invalid/x\n")
        )

    def test_curl_retry_connrefused_alone_does_not_cover_unreachable_connection(self) -> None:
        """Reject refused-only curl retries because other exit-7 causes remain fatal."""
        self.assertEqual(
            self.check_script(
                "curl -o out --retry 3 --retry-connrefused https://example.invalid/x\n"
            ),
            ["fetch.sh:1: curl needs --retry-all-errors"],
        )

    def test_wget_without_tries_fails(self) -> None:
        """Reject wget without an explicit positive attempt count."""
        self.assertTrue(
            self.check_script(
                "wget --retry-connrefused --retry-on-host-error https://example.invalid/x\n"
            )
        )

    def test_wget_without_retry_connrefused_fails(self) -> None:
        """Reject wget without retries for refused or unreachable connections."""
        self.assertTrue(
            self.check_script("wget --tries=3 --retry-on-host-error https://example.invalid/x\n")
        )

    def test_wget_without_retry_on_host_error_fails(self) -> None:
        """Reject wget without host resolution retries."""
        self.assertTrue(
            self.check_script("wget --tries=3 --retry-connrefused https://example.invalid/x\n")
        )

    def test_existing_all_errors_retry_is_accepted(self) -> None:
        """Accept the existing curl all-errors retry convention."""
        self.assertEqual(
            self.check_script(
                "curl -o out -f --retry 5 --retry-all-errors https://example.invalid/x\n"
            ),
            [],
        )

    def test_continuation_flags_count(self) -> None:
        """Find retry flags across supported logical line continuations."""
        for continuation in ("\\", "`", "^"):
            with self.subTest(continuation=continuation):
                self.assertEqual(
                    self.check_script(
                        f"curl -o out -fsSL --retry 3 {continuation}\n"
                        f"  --retry-all-errors {continuation}\n"
                        "  https://example.invalid/x\n"
                    ),
                    [],
                )

    def test_loopback_without_exact_allowlist_entry_fails(self) -> None:
        """Require retries for loopback fetches unless explicitly exempted."""
        self.assertTrue(self.check_script("curl -fsSL http://localhost:8080/healthz\n"))
        self.assertTrue(self.check_script("wget -q http://127.0.0.1:8080/healthz\n"))

    def test_allowlist_is_exact_and_stale_entries_fail(self) -> None:
        """Reject missing, duplicated, or mismatched exact exemptions."""
        entry = (("fetch.sh", "http://localhost:8080/healthz", "bounded outer poll"),)
        source = "curl -fsSL http://localhost:8080/healthz\n"
        self.assertEqual(audit_sources([("fetch.sh", source)], entry), [])
        self.assertTrue(audit_sources([], entry))
        self.assertTrue(audit_sources([("other.sh", source)], entry))
        self.assertTrue(audit_sources([("fetch.sh", source * 2)], entry))
        self.assertTrue(
            audit_sources([("fetch.sh", source.replace("healthz", "healthz/new"))], entry)
        )

    def test_flags_on_next_command_or_comment_do_not_count(self) -> None:
        """Keep comments and subsequent commands outside fetch arguments."""
        for suffix in (" # --retry-all-errors", " | echo --retry-all-errors"):
            with self.subTest(suffix=suffix):
                self.assertTrue(
                    self.check_script("curl -o out --retry 3 https://example.invalid/x" + suffix)
                )

    def test_retry_must_be_enabled_and_positive(self) -> None:
        """Reject missing, nonpositive, noninteger, and disabled curl retries."""
        compliant = "curl -o out --retry 3 --retry-all-errors https://example.invalid/x"
        for mutation in (
            compliant.replace("--retry 3", ""),
            compliant.replace("--retry 3", "--retry 0"),
            compliant.replace("--retry 3", "--retry -1"),
            compliant.replace("--retry 3", "--retry 1.5"),
            compliant.replace("--retry-all-errors", "--retry-all-errors --no-retry-all-errors"),
        ):
            with self.subTest(mutation=mutation):
                self.assertTrue(self.check_script(mutation))

    def test_wget_retry_must_be_enabled_and_positive(self) -> None:
        """Reject zero attempts and disabled wget connection or host retries."""
        compliant = (
            "wget --tries=3 --retry-connrefused --retry-on-host-error https://example.invalid/x"
        )
        for mutation in (
            compliant.replace("--tries=3", "--tries=0"),
            compliant.replace("--retry-connrefused", "--no-retry-connrefused"),
            compliant.replace("--retry-on-host-error", "--no-retry-on-host-error"),
        ):
            with self.subTest(mutation=mutation):
                self.assertTrue(self.check_script(mutation))

    def test_unbalanced_quote_uses_whitespace_split_without_skipping_line(self) -> None:
        """Audit malformed logical lines instead of crashing or silently skipping fetches."""
        for url in ('https://example.invalid/x"', "'https://example.invalid/x"):
            with self.subTest(url=url):
                self.assertEqual(
                    self.check_script(f"curl -o out --retry 3 {url}\n"),
                    ["fetch.sh:1: curl needs --retry-all-errors"],
                )
                self.assertEqual(
                    self.check_script(f"curl -o out --retry 3 --retry-all-errors {url}\n"), []
                )

    def test_fetch_names_in_diagnostic_prose_are_not_commands(self) -> None:
        """Exclude tool names inside diagnostics while retaining real bare-URL fetches."""
        self.assertEqual(
            self.check_script('die 2 "curl or wget is required to download $url"\n'), []
        )
        self.assertTrue(self.check_script("curl https://example.invalid/x\n"))
        self.assertTrue(self.check_script("wget https://example.invalid/x\n"))

    def test_variable_urls_windows_and_container_fetches(self) -> None:
        """Inventory variable URLs in shell, Windows, and container commands."""
        for command in (
            'http_code="$(curl --retry 3 "$URL")"',
            'RUN curl -fsSL "$url" && echo done',
            "curl.exe --url %WINDOWS_BASEKIT_URL% --retry 5",
        ):
            with self.subTest(command=command):
                self.assertTrue(self.check_script(command))

    def test_scanned_file_kinds(self) -> None:
        """Include supported shell and recipe paths while excluding prose files."""
        for path in (
            "scripts/fetch.sh",
            "fetch.bash",
            "scripts/fetch.ps1",
            "fetch.cmd",
            ".github/workflows/fetch.yml",
            ".github/workflows/fetch.yaml",
            "Dockerfile",
            "dev/Containerfile.runner",
            "docker/dev/alpine.Dockerfile",
        ):
            with self.subTest(path=path):
                self.assertTrue(in_scope(path, ""))
        self.assertTrue(in_scope("fetch", "#!/usr/bin/env bash\n"))
        self.assertFalse(in_scope("README.md", "curl https://example.invalid/x"))

    def test_url_options(self) -> None:
        """Audit both long URL spellings and their consumed values."""
        for url in ("--url=https://example.invalid/x", "--url https://example.invalid/x"):
            with self.subTest(url=url):
                self.assertIn(
                    "fetch.sh:1: curl needs positive --retry",
                    self.check_script(f"curl {url} -o out"),
                )
                self.assertEqual(
                    self.check_script(f"curl {url} --retry 3 --retry-all-errors -o out"), []
                )

    def test_option_value_consumption(self) -> None:
        """Keep option-looking values out of the flag state."""
        self.assertEqual(
            self.check_script(
                "curl --output --retry-all-errors --retry 3 https://example.invalid/x"
            ),
            ["fetch.sh:1: curl needs --retry-all-errors"],
        )
        for output in ("-fsSLo out", "--output=out", "-oout"):
            self.assertEqual(
                self.check_script(
                    f"curl {output} --retry=3 --retry-all-errors https://example.invalid/x"
                ),
                [],
            )
        for output in ("-qO out", "-qOout", "--output-document=out"):
            self.assertEqual(
                self.check_script(
                    f"wget {output} --tries=3 --retry-connrefused --retry-on-host-error "
                    "https://example.invalid/x"
                ),
                [],
            )

    def test_invalid_options(self) -> None:
        """Reject unknown options, flag values, and missing option values."""
        base = "curl -o out --retry 3 --retry-all-errors https://example.invalid/x "
        for option, error in (
            ("--retry-all-errors=1", "curl flag --retry-all-errors does not take a value"),
            ("--retry", "curl option --retry needs a value"),
            ("--bogus", "unknown curl option --bogus"),
            ("-Z", "unknown curl option -Z"),
            ("--no-output=out", "unknown curl option --no-output"),
            ("--no-tlsv1.2", "unknown curl option --no-tlsv1.2"),
        ):
            with self.subTest(option=option):
                self.assertIn(f"fetch.sh:1: {error}", self.check_script(base + option))
        self.assertIn(
            "fetch.sh:1: unknown wget option --no-tries",
            self.check_script("wget --no-tries=3 https://example.invalid/x"),
        )

    def test_body_stdout(self) -> None:
        """Reject pipe, capture, redirected-file and explicit stdout bodies."""
        base = "curl --retry 3 --retry-all-errors https://example.invalid/x"
        for command in (
            base + " | tar",
            f'body="$({base})"',
            base + " > file",
            base + " -o -",
            base + " 1>file",
            base + " &>file",
            base + " >>file",
            base + " -o out -o -",
            base + " -I --no-head",
            base + " -O --no-remote-name",
        ):
            with self.subTest(command=command):
                self.assertEqual(
                    self.check_script(command),
                    ["fetch.sh:1: curl retry must write the body to a file, not stdout"],
                )
        self.assertEqual(
            self.check_script(
                "wget -qO- --tries=3 --retry-connrefused --retry-on-host-error "
                "https://example.invalid/x"
            ),
            ["fetch.sh:1: wget retry must write the body to a file, not stdout"],
        )

    def test_body_file_or_discard(self) -> None:
        """Accept files, explicit disposal, remote names and header-only fetches."""
        base = "curl --retry 3 --retry-all-errors https://example.invalid/x"
        for suffix in (
            " >/dev/null",
            " > /dev/null",
            " 1>/dev/null",
            " &>/dev/null",
            " -o /dev/null",
            " -I",
            " -O",
            " -o out 2>errors <input",
            " -o out 2> errors < input",
            " --no-head -I",
            " --no-remote-name -O",
            " -o - -o out",
        ):
            with self.subTest(suffix=suffix):
                self.assertEqual(self.check_script(base + suffix), [])
        self.assertEqual(
            self.check_script(
                "wget -qO- --tries=3 --retry-connrefused --retry-on-host-error "
                "https://example.invalid/x --spider"
            ),
            [],
        )

    def test_comment_fragments(self) -> None:
        """Preserve URL fragments while excluding flags and apostrophes in comments."""
        for url in (
            "https://example.invalid/x#frag",
            "'https://example.invalid/x#frag'",
            '"https://example.invalid/x#frag"',
        ):
            self.assertEqual(
                self.check_script(f"curl --retry 3 {url} --retry-all-errors -o out"), []
            )
        self.assertEqual(
            self.check_script(
                "curl --retry 3 -o out https://example.invalid/x # don't --retry-all-errors"
            ),
            ["fetch.sh:1: curl needs --retry-all-errors"],
        )
        self.assertEqual(strip_comment("a#b # end"), "a#b ")
        self.assertEqual(strip_comment(r"a\#b # end"), r"a\#b ")
        self.assertEqual(strip_comment(r'"a\"#b" # end'), r'"a\"#b" ')
        self.assertEqual(strip_comment("echo ok;# end"), "echo ok;")

    def test_apt_retries(self) -> None:
        """Require positive literal retry settings for tidy-lane update and install."""
        path = "scripts/dev/tidy-lane.sh"
        for verb in ("update", "install -y package"):
            self.assertEqual(
                audit_sources([(path, f"apt-get -o Acquire::Retries=3 {verb}")], ()), []
            )
            for setting in (
                "",
                "-o Acquire::Retries=0",
                "-o Acquire::Retries=x",
                "-o Acquire::Retries=3 -o Acquire::Retries=0",
            ):
                with self.subTest(verb=verb, setting=setting):
                    self.assertEqual(
                        audit_sources([(path, f"apt-get {setting} {verb}")], ()),
                        [f"{path}:1: apt-get needs -o Acquire::Retries=N with N positive"],
                    )
        self.assertEqual(audit_sources([(path, "apt-get --version")], ()), [])
        self.assertEqual(
            audit_sources([(path, "echo ready && apt-get update")], ()),
            [f"{path}:1: apt-get needs -o Acquire::Retries=N with N positive"],
        )
        self.assertEqual(audit_sources([(path, "# apt-get update")], ()), [])

    def test_known_gap_git_clone(self) -> None:
        """Known gap (#3392): Git clone retries remain outside this scanner."""
        self.assertEqual(self.check_script("git clone https://example.invalid/r.git"), [])

    def test_known_gap_other_apt(self) -> None:
        """Known gap (#3392): APT outside tidy-lane remains outside this scanner."""
        for setting in ("", "-o Acquire::Retries=0", "-o Acquire::Retries=x"):
            self.assertEqual(audit_sources([("other.sh", f"apt-get {setting} install x")], ()), [])

    def test_url_exemption(self) -> None:
        """Match exemptions against URLs consumed by long options."""
        entry = (("fetch.sh", "http://localhost:8080/healthz", "bounded outer poll"),)
        for url in ("--url=http://localhost:8080/healthz", "--url http://localhost:8080/healthz"):
            self.assertEqual(audit_sources([("fetch.sh", f"curl {url}")], entry), [])
        self.assertTrue(
            audit_sources([("fetch.sh", "curl --url=http://localhost:8080/other")], entry)
        )

    def test_tracked_fetches_retry_connect_failures(self) -> None:
        """Enforce the retry contract across the live tracked site inventory."""
        self.assertEqual(audit_sources(tracked_sources()), [])


if __name__ == "__main__":
    unittest.main()
