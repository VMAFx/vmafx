# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Go packages that link libvmaf must not document that they do not.

`pkg/libvmaf` imports "C" and links `libvmaf.so` (`ScoreDirect`, `StreamScorer`,
`DNNSession`), and every package importing it links the library too. Its
package documentation and `cmd/vmafx-mcp/tools.go` once said the opposite
("does not link against libvmaf.so at runtime"), which sends an operator who
deploys the binary without the library, or a reviewer reading the linkage,
the wrong way. This contract reads the Go sources: a package that links
libvmaf may not carry such a claim, and the package documentation of
`pkg/libvmaf` names each of its paths into libvmaf.
"""

from __future__ import annotations

import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GIT_TIMEOUT_S = 60
LIBVMAF_IMPORT = '"github.com/VMAFx/vmafx/pkg/libvmaf"'
CGO_IMPORT = re.compile(r'^import "C"\s*$', re.MULTILINE)
CGO_VMAF = re.compile(r"#cgo\b.*\bvmaf\b|#include <libvmaf/", re.MULTILINE)
NO_LINK_CLAIM = re.compile(
    r"(does\s+not|doesn't|never)\s+link(s|ed)?\s+(against|to|with)\s+libvmaf", re.IGNORECASE
)
# The ways pkg/libvmaf reaches libvmaf; its package documentation names each.
PACKAGE_DOC_SURFACES = ("Scorer", "ScoreDirect", "StreamScorer", "DNNSession", "cgo")


def go_sources() -> list[Path]:
    out = subprocess.run(  # noqa: S603 -- fixed argv, repository-local git
        ["git", "-C", str(ROOT), "ls-files", "*.go"],  # noqa: S607
        check=True,
        capture_output=True,
        text=True,
        timeout=GIT_TIMEOUT_S,
    ).stdout
    return [ROOT / line for line in out.splitlines() if line and not line.endswith("_test.go")]


def links_libvmaf(text: str) -> bool:
    if LIBVMAF_IMPORT in text:
        return True
    return bool(CGO_IMPORT.search(text) and CGO_VMAF.search(text))


def linking_packages(files: list[Path]) -> dict[Path, list[Path]]:
    by_dir: dict[Path, list[Path]] = {}
    for path in files:
        by_dir.setdefault(path.parent, []).append(path)
    return {
        directory: members
        for directory, members in by_dir.items()
        if any(links_libvmaf(p.read_text(encoding="utf-8")) for p in members)
    }


def prose(source: str) -> str:
    """Comment text joined across lines, so a claim wrapped over two comment lines is one phrase."""
    return " ".join(re.sub(r"//+|^\s*\*", " ", source, flags=re.MULTILINE).split())


def false_claims(packages: dict[Path, list[Path]]) -> list[str]:
    found = []
    for members in packages.values():
        for path in members:
            if NO_LINK_CLAIM.search(prose(path.read_text(encoding="utf-8"))):
                found.append(str(path.relative_to(ROOT)))
    return sorted(found)


class GoLibvmafLinkageDocs(unittest.TestCase):
    def test_pkg_libvmaf_links_libvmaf(self) -> None:
        packages = linking_packages(go_sources())
        self.assertIn(ROOT / "pkg" / "libvmaf", packages)
        self.assertIn(ROOT / "cmd" / "vmafx-mcp", packages)

    def test_no_linking_package_claims_it_does_not_link(self) -> None:
        self.assertEqual(false_claims(linking_packages(go_sources())), [])

    def test_package_doc_names_every_path_into_libvmaf(self) -> None:
        doc = (ROOT / "pkg" / "libvmaf" / "doc.go").read_text(encoding="utf-8")
        missing = [name for name in PACKAGE_DOC_SURFACES if name not in doc]
        self.assertEqual(missing, [], "pkg/libvmaf/doc.go must name every path into libvmaf")

    def test_claim_detector_sees_a_wrapped_claim(self) -> None:
        planted = "// The server does NOT\n// link against libvmaf.so at runtime.\n"
        self.assertIsNotNone(NO_LINK_CLAIM.search(prose(planted)))
        self.assertIsNone(
            NO_LINK_CLAIM.search(prose("// It links against libvmaf.so through cgo."))
        )


if __name__ == "__main__":
    unittest.main()
