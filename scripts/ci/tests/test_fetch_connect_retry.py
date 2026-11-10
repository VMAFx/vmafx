#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Offline contract for connection retries at every tracked shell fetch site."""

from __future__ import annotations

import re
import shlex
import shutil
import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

# Exact path + URL argument + reason. No implicit loopback exemption: a new
# probe must demonstrate its own bounded poll, or why repeating it is incorrect.
ALLOWLIST = (
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
    sources = []
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
    lines = []
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


def command_args(command: str) -> list[str]:
    """Tokenize one fetch, stopping before the next shell command or enclosing $()."""
    lexer = shlex.shlex(command, posix=True, punctuation_chars=";&|()")
    lexer.whitespace_split = True
    try:
        tokens = list(lexer)
    except ValueError:
        # Embedded shell/YAML fragments may end inside an enclosing quote.
        # Keep auditing the whole fetch rather than discarding that line.
        tokens = [token.strip("\"'") for token in command.split()]
    args = []
    for token in tokens:
        if token in CONTROL or token.startswith("#"):
            break
        args.append(token)
    return args


def fetch_commands(text: str) -> list[tuple[int, list[str]]]:
    """Inventory literal and variable URL fetches on every logical source line."""
    commands = []
    for line_number, line in logical_lines(text):
        if line.lstrip().startswith("#"):
            continue
        for match in FETCH.finditer(line):
            args = command_args(line[match.start() :])
            # Include variable URLs (including Windows %URL%), not just literal
            # hosts. Package lists and command -v checks have no URL arguments.
            if any(re.match(r"https?://|\$|%", arg) for arg in args[1:]):
                commands.append((line_number, args))
    return commands


def option_value(args: list[str], option: str) -> str:
    """Read the last value supplied with a long option."""
    value = ""
    for index, arg in enumerate(args):
        if arg.startswith(option + "="):
            value = arg.partition("=")[2]
        elif arg == option:
            value = args[index + 1] if index + 1 < len(args) else ""
    return value


def positive_count(args: list[str], option: str) -> bool:
    """Accept only literal positive integer retry counts."""
    return bool(re.fullmatch(r"[1-9][0-9]*", option_value(args, option)))


def enabled(args: list[str], option: str) -> bool:
    """Honor the last explicit enable or disable switch."""
    switches = [arg for arg in args if arg in (option, option.replace("--", "--no-", 1))]
    return bool(switches) and switches[-1] == option


def retry_errors(args: list[str]) -> list[str]:
    """Require positive retries and the fetch tool's connection error switches."""
    if args[0] == "wget":
        if (
            positive_count(args, "--tries")
            and enabled(args, "--retry-connrefused")
            and enabled(args, "--retry-on-host-error")
        ):
            return []
        return ["wget needs positive --tries, --retry-connrefused and --retry-on-host-error"]
    errors: list[str] = []
    if not positive_count(args, "--retry"):
        errors.append("curl needs positive --retry")
    if not enabled(args, "--retry-all-errors"):
        errors.append("curl needs --retry-all-errors")
    return errors


def matching_exemptions(
    path: str, args: list[str], allowlist: tuple[tuple[str, str, str], ...]
) -> list[tuple[str, str, str]]:
    """Match exemptions by both tracked path and complete URL argument."""
    return [entry for entry in allowlist if entry[0] == path and entry[1] in args]


def audit_sources(
    sources: list[tuple[str, str]], allowlist: tuple[tuple[str, str, str], ...] = ALLOWLIST
) -> list[str]:
    """Audit fetch arguments and reject stale or ambiguous allow-list entries."""
    errors: list[str] = []
    seen: Counter[tuple[str, str, str]] = Counter()
    for path, text in sources:
        for line, args in fetch_commands(text):
            matches = matching_exemptions(path, args, allowlist)
            if matches:
                seen.update(matches)
            else:
                errors.extend(f"{path}:{line}: {error}" for error in retry_errors(args))
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
                "curl -fsSL --retry 3 --retry-all-errors https://example.invalid/x\n"
                "wget -q --tries=3 --retry-connrefused --retry-on-host-error "
                "https://example.invalid/x\n"
            ),
            [],
        )

    def test_plain_retry_does_not_cover_refused_connection(self) -> None:
        """Reject curl retries that omit all-errors coverage."""
        self.assertTrue(self.check_script("curl -fsSL --retry 3 https://example.invalid/x\n"))

    def test_curl_retry_connrefused_alone_does_not_cover_unreachable_connection(self) -> None:
        """Reject refused-only curl retries because other exit-7 causes remain fatal."""
        self.assertEqual(
            self.check_script("curl --retry 3 --retry-connrefused https://example.invalid/x\n"),
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
            self.check_script("curl -f --retry 5 --retry-all-errors https://example.invalid/x\n"),
            [],
        )

    def test_continuation_flags_count(self) -> None:
        """Find retry flags across supported logical line continuations."""
        for continuation in ("\\", "`", "^"):
            with self.subTest(continuation=continuation):
                self.assertEqual(
                    self.check_script(
                        f"curl -fsSL --retry 3 {continuation}\n"
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
                    self.check_script("curl --retry 3 https://example.invalid/x" + suffix)
                )

    def test_retry_must_be_enabled_and_positive(self) -> None:
        """Reject missing, nonpositive, noninteger, and disabled curl retries."""
        compliant = "curl --retry 3 --retry-all-errors https://example.invalid/x"
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
                    self.check_script(f"curl --retry 3 {url}\n"),
                    ["fetch.sh:1: curl needs --retry-all-errors"],
                )
                self.assertEqual(
                    self.check_script(f"curl --retry 3 --retry-all-errors {url}\n"), []
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

    def test_tracked_fetches_retry_connect_failures(self) -> None:
        """Enforce the retry contract across the live tracked site inventory."""
        self.assertEqual(audit_sources(tracked_sources()), [])


if __name__ == "__main__":
    unittest.main()
