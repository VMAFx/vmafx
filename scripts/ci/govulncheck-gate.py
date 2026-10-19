#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Go vulnerability gate: govulncheck at symbol level, with OpenVEX for the rest.

Runs govulncheck (``GOVULNCHECK_VERSION`` of build-config.env, through
``go run``) with ``-scan symbol -format json`` over the module and sorts its
findings by the deepest frame of each trace:

* a frame that names a function: vmafx calls the vulnerable symbol. The gate
  fails; update the dependency or stop calling it.
* a package or a module only: the vulnerable code is required or imported but
  never called. The gate fails unless ``security/vex/go.openvex.json`` holds a
  ``not_affected`` statement for that advisory, so every such finding carries a
  recorded justification. A statement that says the code is not present
  (``vulnerable_code_not_present``, ``component_not_present``) covers a
  module-level finding only; once a package of the advisory is imported it is
  false, and the gate says so.

govulncheck itself failing (a package that does not load, a cgo setup that is
missing) is exit 2, never a pass: a scan that did not run reports nothing.

Exit 0 clean, 1 a finding, 2 the scan did not complete.
Environment: ``GOVULNCHECK`` runs that executable instead of ``go run`` (tests).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "build-config.env"
VEX = ROOT / "security" / "vex" / "go.openvex.json"
MODULE = "golang.org/x/vuln/cmd/govulncheck"


def configured_version(config: Path = CONFIG) -> str:
    match = re.search(r'^GOVULNCHECK_VERSION="(v\d+\.\d+\.\d+)"', config.read_text("utf-8"), re.M)
    if match is None:
        raise ValueError(f'{config}: no GOVULNCHECK_VERSION="vX.Y.Z"')
    return match.group(1)


def command(packages: list[str]) -> list[str]:
    args = ["-scan", "symbol", "-format", "json", *packages]
    stub = os.environ.get("GOVULNCHECK")
    if stub:
        return [stub, *args]
    go = shutil.which("go") or "go"
    return [go, "run", f"{MODULE}@{configured_version()}", *args]


def messages(stream: str) -> list[dict[str, Any]]:
    """govulncheck's JSON output is a sequence of concatenated objects."""
    decoder = json.JSONDecoder()
    found: list[dict[str, Any]] = []
    index = 0
    while index < len(stream):
        while index < len(stream) and stream[index].isspace():
            index += 1
        if index >= len(stream):
            break
        value, index = decoder.raw_decode(stream, index)
        found.append(value)
    return found


def level(finding: dict[str, Any]) -> str:
    """symbol, package or module: what the deepest frame names."""
    frame = (finding.get("trace") or [{}])[0]
    if frame.get("function"):
        return "symbol"
    return "package" if frame.get("package") else "module"


#: Justifications that claim the vulnerable code is absent from the build: they
#: cannot cover a finding whose trace reaches a package.
ABSENT = frozenset({"component_not_present", "vulnerable_code_not_present"})


def vex_not_affected(path: Path = VEX) -> dict[str, str]:
    """Advisory -> justification of every not_affected statement."""
    if not path.is_file():
        return {}
    document = json.loads(path.read_text("utf-8"))
    return {
        statement["vulnerability"]["name"]: statement.get("justification", "")
        for statement in document.get("statements", [])
        if statement.get("status") == "not_affected"
    }


def verdict(osv: str, kind: str, where: str, justified: dict[str, str]) -> tuple[bool, str]:
    """(fails, text) for one finding."""
    if kind == "symbol":
        return True, f"{osv}: vmafx calls {where}"
    if osv not in justified:
        return True, f"{osv}: {kind} {where} has no not_affected statement in {VEX.name}"
    if kind == "package" and justified[osv] in ABSENT:
        return True, (
            f"{osv}: package {where} is imported, but {VEX.name} justifies it as "
            f"{justified[osv]}; record what keeps it from being called"
        )
    return False, f"{osv}: {kind} {where} not called; {justified[osv]} in {VEX.name}"


_DEPTH = {"module": 0, "package": 1, "symbol": 2}


def deepest(output: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """Advisory -> its most specific finding: govulncheck reports one per level."""
    chosen: dict[str, dict[str, Any]] = {}
    for message in output:
        finding = message.get("finding")
        if not finding:
            continue
        best = chosen.get(finding["osv"])
        if best is None or _DEPTH[level(finding)] > _DEPTH[level(best)]:
            chosen[finding["osv"]] = finding
    return chosen


def judge(output: list[dict[str, Any]], justified: dict[str, str]) -> tuple[list[str], list[str]]:
    """(failures, notes): one verdict per advisory, on its most specific finding."""
    failures: list[str] = []
    notes: list[str] = []
    for osv, finding in sorted(deepest(output).items()):
        frame = (finding.get("trace") or [{}])[0]
        kind = level(finding)
        where = frame.get("module", "?")
        if kind == "package":
            where = frame["package"]
        elif kind == "symbol":
            where = f"{frame.get('package', '')}.{frame['function']}"
        fails, text = verdict(osv, kind, where, justified)
        (failures if fails else notes).append(text)
    return failures, notes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("packages", nargs="*", default=["./..."])
    parser.add_argument(
        "--dir", type=Path, default=ROOT, help="module to scan (default: the repository)"
    )
    args = parser.parse_args(argv)
    try:
        result = subprocess.run(  # noqa: S603 -- fixed argv
            command(args.packages), cwd=args.dir, capture_output=True, text=True, check=False
        )
        output = messages(result.stdout)
    except (OSError, ValueError) as error:
        print(f"govulncheck-gate: the scan did not complete: {error}", file=sys.stderr)
        return 2
    if result.returncode != 0 or not any("config" in m for m in output):
        sys.stderr.write(result.stderr)
        print(f"govulncheck-gate: govulncheck exited {result.returncode}", file=sys.stderr)
        return 2
    failures, notes = judge(output, vex_not_affected())
    for line in notes:
        print(f"govulncheck-gate: {line}")
    for line in failures:
        print(f"govulncheck-gate: {line}", file=sys.stderr)
    if failures:
        return 1
    print(f"govulncheck-gate: OK (no reachable vulnerable symbol, {len(notes)} justified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
