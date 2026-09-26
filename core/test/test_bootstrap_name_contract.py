#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Keep bootstrap score-name literals owned by the shared internal header."""

from __future__ import annotations

import unittest
from pathlib import Path

SRC_DIR = Path(__file__).resolve().parents[1] / "src"
HEADER = SRC_DIR / "bootstrap_names.h"
CONSUMERS = (SRC_DIR / "libvmaf.c", SRC_DIR / "predict.c")
SUFFIX_SYMBOLS = {
    "_bagging": "BOOTSTRAP_SUFFIX_BAGGING",
    "_stddev": "BOOTSTRAP_SUFFIX_STDDEV",
    "_ci_p95_lo": "BOOTSTRAP_SUFFIX_CI_LO",
    "_ci_p95_hi": "BOOTSTRAP_SUFFIX_CI_HI",
}


class BootstrapNameContractTest(unittest.TestCase):
    """Pin ADR-0480's one-owner contract across both score paths."""

    def test_header_owns_suffix_literals(self) -> None:
        header = HEADER.read_text(encoding="utf-8")

        for suffix, symbol in SUFFIX_SYMBOLS.items():
            self.assertIn(f'#define {symbol} "{suffix}"', header)
        self.assertIn("#define BOOTSTRAP_NAME_BUF_SZ(collection_name)", header)

    def test_consumers_use_shared_symbols(self) -> None:
        for path in CONSUMERS:
            with self.subTest(path=path.name):
                source = path.read_text(encoding="utf-8")
                self.assertTrue(
                    '#include "bootstrap_names.h"' in source,
                    f"{path.name} must include the shared bootstrap-name owner",
                )
                self.assertTrue(
                    "BOOTSTRAP_NAME_BUF_SZ(" in source,
                    f"{path.name} must use shared bootstrap-name sizing",
                )
                for suffix, symbol in SUFFIX_SYMBOLS.items():
                    self.assertTrue(
                        f'"{suffix}"' not in source,
                        f"{path.name} must not redeclare {suffix}",
                    )
                    self.assertTrue(
                        symbol in source,
                        f"{path.name} must consume {symbol}",
                    )


if __name__ == "__main__":
    unittest.main()
