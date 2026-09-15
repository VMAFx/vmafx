#!/usr/bin/env python3
"""Agent PreToolUse evasion interceptor (HISS-16).

Wire this script as a PreToolUse hook of the agent harness. The harness passes the
pending tool call as JSON on stdin (the shell command lives at tool_input.command) or
as argv. Exit code 2 blocks the call and returns the reason to the agent.

A git hook cannot observe --no-verify because git skips hooks entirely, so this script
is deliberately not part of lefthook.yml.
"""
import json
import os
import re
import sys

BLOCKED_PATTERNS = [
    r"--no-verify\b",
    r"\bgit\s+commit\b.*\s-n\b",
    r"LEFTHOOK=0\b",
    r"SKIP=.*git",
    r"core\.hooksPath\s*=\s*/dev/null",
    r"rm\s+(-rf?\s+)?\.git/hooks",
]

BLOCK_EXIT = 2


def pending_command():
    """Return the command under review from argv or the JSON hook payload."""
    if len(sys.argv) > 1:
        return " ".join(sys.argv[1:])
    if sys.stdin.isatty():
        return ""
    raw = sys.stdin.read()
    if not raw.strip():
        return ""
    try:
        payload = json.loads(raw)
    except ValueError:
        return raw
    tool_input = payload.get("tool_input") or {}
    return str(tool_input.get("command", ""))


def main():
    if os.environ.get("LEFTHOOK") == "0":
        sys.stderr.write("[BLOCKED BY HISS-16] LEFTHOOK=0 detected in environment.\n")
        sys.exit(BLOCK_EXIT)
    cmd = pending_command()
    for pattern in BLOCKED_PATTERNS:
        if re.search(pattern, cmd):
            sys.stderr.write(f"[BLOCKED BY HISS-16] Verification evasion prohibited: {pattern}\n")
            sys.exit(BLOCK_EXIT)
    sys.exit(0)


if __name__ == "__main__":
    main()
