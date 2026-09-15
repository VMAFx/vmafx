#!/usr/bin/env python3
import sys, os, re

BLOCKED_PATTERNS = [
    r"--no-verify\b",
    r"-n\b(?=.*git\s+commit)",
    r"LEFTHOOK=0\b",
    r"SKIP=.*git",
    r"core\.hooksPath\s*=\s*/dev/null",
    r"rm\s+(-rf?\s+)?\.git/hooks",
]

def main():
    if os.environ.get("LEFTHOOK") == "0":
        sys.stderr.write("[BLOCKED BY HISS-16] LEFTHOOK=0 detected in environment.\n")
        sys.exit(1)
    if len(sys.argv) > 1:
        cmd = " ".join(sys.argv[1:])
        for p in BLOCKED_PATTERNS:
            if re.search(p, cmd):
                sys.stderr.write(f"[BLOCKED BY HISS-16] Verification evasion prohibited: {p}\n")
                sys.exit(1)
    sys.exit(0)

if __name__ == "__main__":
    main()
