#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
"""Agent PreToolUse evasion interceptor (HISS), written by praetorctl adopt.

Wire this script as a PreToolUse hook of the agent harness's shell tool. The harness passes
the pending tool call as one JSON object on stdin with the shell command at
tool_input.command; a command may instead be passed as arguments. Exit code 2 blocks the
call and returns the reason to the agent. Input of any other shape is refused, not allowed.

The rules and their refusals are the engine's built-in command policy (internal/agenthook),
rendered at adoption; operator rules belong in hooks.command_policy.deny of .standards.yaml.

A git hook cannot observe --no-verify because git skips hooks entirely, so this script
is deliberately not part of lefthook.yml.
"""

import json
import os
import re
import sys

MAX_INPUT_BYTES = 1048576
MAX_SCAN_CHARS = 65536
MAX_SCAN_LINE_CHARS = 2048
BLOCK_EXIT = 2

NO_INPUT_REFUSAL = (
    "[BLOCKED BY HISS] input: PreToolUse JSON " "on stdin or command arguments required"
)
INVALID_INPUT_REFUSAL = "[BLOCKED BY HISS] Invalid hook input: "
NARROWING_REFUSAL = "[BLOCKED BY HISS] hook exclusions: prohibited."
SCAN_BOUND_REFUSAL = (
    "[BLOCKED BY HISS] command exceeds scan bound: at most 65536 characters, 2048 per "
    "line; split command or write long content to file first"
)
HISS_REFUSAL = (
    "[BLOCKED BY HISS] verification evasion prohibited; commits, pushes and tool calls "
    "pass verification gates; pattern: "
)
DEV_01_REFUSAL = (
    "[BLOCKED BY DEV-01] adoption or needs target: workstation dev root; repositories "
    "live inside organization folders as leaf Git repositories; pattern: "
)

RULES = [
    (r"--no-v(e(r(i(f(y)?)?)?)?)?\b", HISS_REFUSAL),
    (
        r"\bgit([ \t]+-[Cc][ \t]+(\x22[^\x22]*\x22|\x27[^\x27]*\x27|[^ \t\n\x22\x27][^"
        r" \t\n]*)|[ \t]+(--[A-Za-z][-A-Za-z]*|-[ABD-Zabd-z][-A-Za-z]*|-[Cc][A-Za-z]+)"
        r"(=[^ \t\n]+)?)*\s+(commit\b[^\n]*\s-[aeiopqsvz]*|am\b[^\n]*\s-[3cikmqsu]*)n",
        HISS_REFUSAL,
    ),
    (r"LEFTHOOK=[\x22\x27]?(0|false)\b", HISS_REFUSAL),
    (r"SKIP=.*git", HISS_REFUSAL),
    (r"(?i:core\.hookspath)(\s*=|\s+[\x22\x27]?[/~.$A-Za-z_\\])", HISS_REFUSAL),
    (
        r"\b(?i:rm|rmdir|unlink|mv|cp|ln|chmod|chown|chattr|truncate|shred|tee|del|era"
        r"se|rd|ri|remove-item|move|move-item|ren|rename|rename-item|copy|copy-item|se"
        r"t-content|add-content|out-file|icacls|attrib)\b[^\n]*(?i:\.git[/\\]hooks)",
        HISS_REFUSAL,
    ),
    (
        r"(?m)^(?:(?:[^\Wsp]\w*|s(?:[^\We]\w*|e(?:[^\Wd]\w*|d\w+)?)?|p(?:[^\We]\w*|e(?"
        r":[^\Wr]\w*|r(?:[^\Wl]\w*|l\w+)?)?)?)?[^\w\n])*(sed|perl)\b[^\n]*\s(-[A-Za-z]"
        r"*i|--in-place)[^\n]*(?i:\.git[/\\]hooks)",
        HISS_REFUSAL,
    ),
    (
        r"(?m)^(?:(?:[^\Wf]\w*|f(?:[^\Wi]\w*|i(?:[^\Wn]\w*|n(?:[^\Wd]\w*|d\w+)?)?)?)?["
        r"^\w\n])*(find)\b[^\n]*(?i:\.git[/\\]hooks)[^\n]*\s-(delete|exec|execdir|ok)"
        r"\b",
        HISS_REFUSAL,
    ),
    (r">\s*[\x22\x27]?[^ \t\n\x22\x27]*(?i:\.git[/\\]hooks)", HISS_REFUSAL),
    (r"\blefthook\s+uninstall\b", HISS_REFUSAL),
    (
        r"(?m)^(?:(?:[^\Wg]\w*|g(?:[^\Wh]\w*|h\w+)?)?[^\w\n])*(gh)\b[^\n]*(?:\bpr\s+me"
        r"rge\b[^\n]*(?:\s|[\x22\x27])--admin(?:\s|$|[;&|\x22\x27]|=true\b)|(?:\s|[\x2"
        r"2\x27])--admin(?:\s|$|[;&|\x22\x27]|=true\b)[^\n]*\bpr\s+merge\b|\bpr\s+--ad"
        r"min(?:\s|$|[;&|\x22\x27]|=true\b)[^\n]*\bmerge\b)",
        HISS_REFUSAL,
    ),
    (
        r"(?m)^(?:(?:[^\Wg]\w*|g(?:[^\Wh]\w*|h\w+)?)?[^\w\n])*(gh)\b[^\n]*\bapi\b[^\n]"
        r"*\bpulls/[0-9]+/merge/?(?:\?|\s|$|[;&|\x22\x27])",
        HISS_REFUSAL,
    ),
    (
        r"(?m)^(?:(?:[^\Wg]\w*|g(?:[^\Wh]\w*|h\w+)?)?[^\w\n])*(gh)\b[^\n]*\bapi\s+grap"
        r"hql\b[^\n]*\bmergePullRequest\b",
        HISS_REFUSAL,
    ),
    (
        r"(?m)^(?:(?:[^\Wcw]\w*|c(?:[^\Wu]\w*|u(?:[^\Wr]\w*|r(?:[^\Wl]\w*|l\w+)?)?)?|w"
        r"(?:[^\Wg]\w*|g(?:[^\We]\w*|e(?:[^\Wt]\w*|t\w+)?)?)?)?[^\w\n])*(curl|wget)\b["
        r"^\n]*\bapi\.github\.com/[^\n]*\bpulls/[0-9]+/merge/?(?:\?|\s|$|[;&|\x22\x27]"
        r")",
        HISS_REFUSAL,
    ),
    (
        r"(?i)(standardsctl|praetorctl)\s+(adopt|conform|bootstrap|needs\s+(scan|repor"
        r"t|migrate|epic))\b.*\bdev[/\\]?(\s|$)",
        DEV_01_REFUSAL,
    ),
]

# The RULES judged without the words of read-only commands chained after a commit. Each
# pattern has a name of its own: wrapped inside the list, it would read as two items with
# the comma between them forgotten.
READ_ONLY_EXEMPT_1 = (
    r"\bgit([ \t]+-[Cc][ \t]+(\x22[^\x22]*\x22|\x27[^\x27]*\x27|[^ \t\n\x22\x27][^ \t\n"
    r"]*)|[ \t]+(--[A-Za-z][-A-Za-z]*|-[ABD-Zabd-z][-A-Za-z]*|-[Cc][A-Za-z]+)(=[^ \t\n]"
    r"+)?)*\s+(commit\b[^\n]*\s-[aeiopqsvz]*|am\b[^\n]*\s-[3cikmqsu]*)n"
)
READ_ONLY_EXEMPT_2 = r"SKIP=.*git"
READ_ONLY_EXEMPT = [
    READ_ONLY_EXEMPT_1,
    READ_ONLY_EXEMPT_2,
]
READ_ONLY_WORDS = (
    r"((?:;|&&|\|\|?)[ \t]*(?:git[ \t]+(?:--no-pager[ \t]+)?(?:log|show|status|diff|rev"
    r"-parse)|head|tail|grep|wc))(?:[ \t]+[-A-Za-z0-9_./:=@,+~*?]+)*|((?:;|&&|\|\|?)[ "
    r"\t]*sed)[ \t]+-n\b(?:[ \t]+[-A-Za-z0-9_./:=@,+~*?]+)*"
)
READ_ONLY_VETO = (
    r"[^\t\x20-\x7e]|[\\\x60$(){}<>^!]|--%|%[;&|]|(?i:(?:\A|[^-A-Za-z0-9_])(?:export|de"
    r"clare|typeset|set|setenv|alias|function|sal|nal|set-alias|new-alias|doskey|hash)("
    r"?:[^-A-Za-z0-9_]|$))"
)

LEFTHOOK_DISABLED = [
    ("0", "[BLOCKED BY HISS] LEFTHOOK=0 detected in environment. Evasion prohibited."),
    (
        "false",
        "[BLOCKED BY HISS] LEFTHOOK=false detected in environment. Evasion prohibited.",
    ),
]
LEFTHOOK_NARROWING = ("LEFTHOOK_EXCLUDE", "LEFTHOOK_SKIP")


class Blocked(Exception):
    pass


def read_payload_command(stream):
    raw = stream.read(MAX_INPUT_BYTES + 1)
    if len(raw) > MAX_INPUT_BYTES:
        raise ValueError("hook input exceeds " + str(MAX_INPUT_BYTES) + " bytes")
    payload = json.loads(raw.decode("utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("hook input must be one JSON object")
    tool_input = payload.get("tool_input")
    if not isinstance(tool_input, dict):
        raise ValueError("tool_input must be an object")
    command = tool_input.get("command")
    if not isinstance(command, str) or not command.strip():
        raise ValueError("tool_input.command must be nonempty text")
    return command


def pending_command():
    if len(sys.argv) > 1:
        return " ".join(sys.argv[1:])
    if sys.stdin.isatty():
        raise Blocked(NO_INPUT_REFUSAL)
    try:
        return read_payload_command(sys.stdin.buffer)
    except (ValueError, OSError, RecursionError) as error:
        raise Blocked(INVALID_INPUT_REFUSAL + str(error)) from error


def check_environment(environ):
    value = environ.get("LEFTHOOK")
    for disabled, refusal in LEFTHOOK_DISABLED:
        if value == disabled:
            raise Blocked(refusal)
    for name in LEFTHOOK_NARROWING:
        if environ.get(name):
            raise Blocked(NARROWING_REFUSAL)


def without_read_only_words(command):
    # The words of git log, head, sed -n and the other read-only commands READ_ONLY_WORDS
    # names, chained after ;, &&, || or |, belong to that command and are dropped, its name
    # kept. A command holding a construct READ_ONLY_VETO names keeps every word.
    if re.search(READ_ONLY_VETO, command):
        return command
    return re.sub(READ_ONLY_WORDS, r"\1\2", command)


def join_line_continuations(command):
    # A backslash followed by a newline (LF or CRLF) joins lines without regex backtracking.
    return re.sub(r"\\\r?\n", " ", command)


# The git rule with an option run (the short skip flag rule) is searched once per run (#1283).
# re retries a rule at every git it finds, and a git inside an option token (-git, --x=git,
# -C dir/git) starts at a boundary of the run an earlier git already walked: each retry walked
# the rest of that run again, quadratic in the line. A start at a walked boundary reaches the
# same end, and only a run's end can be followed by the rest of the rule (every option starts
# with a blank and a dash), so skipping it keeps re.search's answer. The adopter interceptor
# carries this code byte for byte (TestEmittedInterceptorSearchesLikeTheEngineGuard).
GIT_OPTION_RUN_HEAD = r"\bgit("
GIT_OPTION_RUN_JOIN = r")*\s+("


def search(pattern, text):
    """re.search(pattern, text) is not None, with the git option-run rule in linear time."""
    head, join, tail = pattern.partition(GIT_OPTION_RUN_JOIN)
    if not join or not head.startswith(GIT_OPTION_RUN_HEAD):
        return re.search(pattern, text) is not None
    first = len(GIT_OPTION_RUN_HEAD)
    option = re.compile(head[first:])
    rest = re.compile(r"\s+(" + tail)
    walked = set()
    for found in re.finditer(r"\bgit", text):
        position = found.end()
        if position in walked:
            continue
        while position not in walked:
            walked.add(position)
            step = option.match(text, position)
            if step is None or step.end() == position:
                break
            position = step.end()
        if rest.match(text, position):
            return True
    return False


def check_command(command):
    command = join_line_continuations(command)
    # re backtracks, so a longer command or line could stall this script past the harness's
    # hook timeout. Such a command is refused, never truncated.
    if len(command) > MAX_SCAN_CHARS:
        raise Blocked(SCAN_BOUND_REFUSAL)
    if max(len(line) for line in command.split("\n")) > MAX_SCAN_LINE_CHARS:
        raise Blocked(SCAN_BOUND_REFUSAL)
    judged = without_read_only_words(command)
    for pattern, refusal in RULES:
        text = judged if pattern in READ_ONLY_EXEMPT else command
        if search(pattern, text):
            raise Blocked(refusal + pattern)


def main():
    try:
        check_environment(os.environ)
        check_command(pending_command())
    except Blocked as blocked:
        sys.stderr.write(str(blocked) + "\n")
        sys.exit(BLOCK_EXIT)
    sys.exit(0)


if __name__ == "__main__":
    main()
