#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/ci/govulncheck-gate.py against a stand-in govulncheck.

The stand-in prints recorded govulncheck v1.8.0 JSON (``-format json``) and
exits as told, so every branch of the gate runs without the network: a called
symbol fails, an uncalled advisory needs an OpenVEX statement, a "not present"
statement cannot cover an imported package, and a scan that did not complete
is exit 2. The real tool's run against a module that calls a vulnerable symbol
is recorded in docs/development/dependency-advisories.md.
"""

from __future__ import annotations

import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GATE = ROOT / "scripts/ci/govulncheck-gate.py"
CONFIG: dict[str, object] = {
    "config": {"scanner_name": "govulncheck", "scanner_version": "v1.8.0", "scan_level": "symbol"}
}


def finding(osv: str, **frame: str) -> dict[str, object]:
    return {"finding": {"osv": osv, "trace": [frame]}}


class GovulncheckGate(unittest.TestCase):
    def run_gate(
        self, messages: list[dict[str, object]], status: int = 0
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as tmp:
            stub = Path(tmp) / "govulncheck"
            payload = Path(tmp) / "out.json"
            payload.write_text("\n".join(json.dumps(m) for m in messages), encoding="utf-8")
            stub.write_text(f"#!/bin/sh\ncat '{payload}'\nexit {status}\n", encoding="utf-8")
            stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
            env = dict(os.environ, GOVULNCHECK=str(stub))
            return subprocess.run(  # noqa: S603 -- the gate under test
                [sys.executable, str(GATE)], capture_output=True, text=True, env=env, check=False
            )

    def test_clean_scan_passes(self) -> None:
        result = self.run_gate([CONFIG])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK", result.stdout)

    def test_called_symbol_fails_even_with_a_statement(self) -> None:
        call = finding(
            "GO-2026-5932",
            module="golang.org/x/crypto",
            package="golang.org/x/crypto/openpgp",
            function="ReadMessage",
        )
        result = self.run_gate([CONFIG, call])
        self.assertEqual(result.returncode, 1)
        self.assertIn("vmafx calls golang.org/x/crypto/openpgp.ReadMessage", result.stderr)

    def test_module_level_finding_with_statement_passes(self) -> None:
        result = self.run_gate(
            [CONFIG, finding("GO-2026-5932", module="golang.org/x/crypto", version="v0.57.0")]
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("GO-2026-5932: module golang.org/x/crypto not called", result.stdout)

    def test_unjustified_advisory_fails(self) -> None:
        result = self.run_gate([CONFIG, finding("GO-2099-0001", module="example.com/x")])
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "GO-2099-0001: module example.com/x has no not_affected statement", result.stderr
        )

    def test_not_present_statement_cannot_cover_an_imported_package(self) -> None:
        imported = finding(
            "GO-2026-5932", module="golang.org/x/crypto", package="golang.org/x/crypto/openpgp"
        )
        result = self.run_gate([CONFIG, imported])
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "is imported, but go.openvex.json justifies it as vulnerable_code_not_present",
            result.stderr,
        )

    def test_a_failed_scan_is_not_a_pass(self) -> None:
        self.assertEqual(self.run_gate([CONFIG], status=1).returncode, 2)
        self.assertEqual(self.run_gate([], status=0).returncode, 2)


if __name__ == "__main__":
    unittest.main()
