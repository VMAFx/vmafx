#!/usr/bin/env python3
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
        r"\b(sed|perl)\b[^\n]*\s(-[A-Za-z]*i|--in-place)[^\n]*(?i:\.git[/\\]hooks)",
        HISS_REFUSAL,
    ),
    (
        r"\bfind\b[^\n]*(?i:\.git[/\\]hooks)[^\n]*\s-(delete|exec|execdir|ok)\b",
        HISS_REFUSAL,
    ),
    (r">\s*[\x22\x27]?[^ \t\n\x22\x27]*(?i:\.git[/\\]hooks)", HISS_REFUSAL),
    (r"\blefthook\s+uninstall\b", HISS_REFUSAL),
    (
        r"(?i)(standardsctl|praetorctl)\s+(adopt|conform|bootstrap|needs\s+(scan|repor"
        r"t|migrate|epic))\b.*\bdev/?(\s|$)",
        DEV_01_REFUSAL,
    ),
]

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
        raise Blocked(INVALID_INPUT_REFUSAL + str(error))


def check_environment(environ):
    value = environ.get("LEFTHOOK")
    for disabled, refusal in LEFTHOOK_DISABLED:
        if value == disabled:
            raise Blocked(refusal)
    for name in LEFTHOOK_NARROWING:
        if environ.get(name):
            raise Blocked(NARROWING_REFUSAL)


def check_command(command):
    # re backtracks, so a longer command or line could stall this script past the harness's
    # hook timeout. Such a command is refused, never truncated.
    if len(command) > MAX_SCAN_CHARS:
        raise Blocked(SCAN_BOUND_REFUSAL)
    if max(len(line) for line in command.split("\n")) > MAX_SCAN_LINE_CHARS:
        raise Blocked(SCAN_BOUND_REFUSAL)
    for pattern, refusal in RULES:
        if re.search(pattern, command):
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
