#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The tester workflows sign with a suffix OpenSSF Scorecard counts as a signature.

Scorecard's Signed-Releases check (v5.6.0, probes/releasesAreSigned) treats a
release asset as a signature only when its name ends in one of
SCORECARD_SIGNATURE_SUFFIXES. The tester prereleases are releases to that
check; a `.bundle` suffix left every one of them "not signed" and moved the
master Scorecard aggregate under its 8.5 floor on 2026-10-04.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github/workflows"

# probes/releasesAreSigned/impl.go, signatureExtensions, Scorecard v5.6.0.
SCORECARD_SIGNATURE_SUFFIXES = (".asc", ".minisig", ".sig", ".sign", ".sigstore", ".sigstore.json")
TESTER_WORKFLOWS = ("macos-tester-bundle.yml", "windows-tester-bundle.yml")
SIGN_BLOB = re.compile(r'cosign sign-blob[^\n]*--bundle "\$f(?P<suffix>[^"]*)"')


def signature_suffixes(text: str) -> list[str]:
    """Return the suffix each `cosign sign-blob --bundle "$f<suffix>"` writes."""
    return [match.group("suffix") for match in SIGN_BLOB.finditer(text)]


class TesterSignatureExtensionTests(unittest.TestCase):
    def test_every_tester_workflow_signs_with_a_scorecard_suffix(self) -> None:
        for name in TESTER_WORKFLOWS:
            suffixes = signature_suffixes((WORKFLOWS / name).read_text())
            self.assertTrue(suffixes, f"{name}: no cosign sign-blob --bundle step found")
            for suffix in suffixes:
                self.assertTrue(
                    suffix.endswith(SCORECARD_SIGNATURE_SUFFIXES),
                    f"{name}: signature suffix {suffix!r} is not one Scorecard counts",
                )

    def test_negative_a_bundle_suffix_is_refused(self) -> None:
        text = 'cosign sign-blob --yes --bundle "$f.bundle" "$f"'
        self.assertEqual(signature_suffixes(text), [".bundle"])
        self.assertFalse(".bundle".endswith(SCORECARD_SIGNATURE_SUFFIXES))

    def test_boundary_sigstore_json_is_accepted(self) -> None:
        text = 'cosign sign-blob --yes --bundle "$f.sigstore.json" "$f"'
        self.assertEqual(signature_suffixes(text), [".sigstore.json"])
        self.assertTrue(".sigstore.json".endswith(SCORECARD_SIGNATURE_SUFFIXES))


if __name__ == "__main__":
    unittest.main()
