#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Device-free contract of the second ADM viewing distance (ADR-2795).

- The shared helpers (adm_view_dist.c, used by every C-side ADM descriptor)
  and the Rust twin (score.rs) file the second distance's scores under the
  same seven keys.
- vmaf_adm_merge_view_dist() compares two contexts by their feature names, which
  carry only the options flagged VMAF_OPT_FLAG_FEATURE_PARAM, and checks the
  others itself. The table's options without the flag must stay exactly the
  ones it handles: debug, adm_skip_aim and adm_norm_view_dist_extra. A new
  option without the flag would be merged across silently.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
C_SRC = ROOT / "core/src/feature/integer_adm.c"
# Every ADM descriptor that merges through adm_view_dist.c: its option table.
MERGING_TABLES = {
    "adm": (C_SRC, "options"),
    "adm_cuda": (ROOT / "core/src/feature/cuda/integer_adm_cuda.c", "options_cuda"),
    "adm_sycl": (ROOT / "core/src/feature/sycl/integer_adm_sycl.cpp", "options"),
    "adm_hip": (ROOT / "core/src/feature/hip/integer_adm_hip.c", "options_hip"),
}
VIEW_SRC = ROOT / "core/src/feature/adm_view_dist.c"
VIEW_HDR = ROOT / "core/src/feature/adm_view_dist.h"
RUST_SCORE = ROOT / "core/src/rust/feature/adm/src/score.rs"
HANDLED_NON_PARAMS = {"debug", "adm_skip_aim", "adm_norm_view_dist_extra"}


def c_text() -> str:
    return C_SRC.read_text(encoding="utf-8")


def view_text() -> str:
    return VIEW_HDR.read_text(encoding="utf-8") + VIEW_SRC.read_text(encoding="utf-8")


def c_extra_keys(text: str) -> list[str]:
    suffix = re.search(r'#define VMAF_ADM_EXTRA_VIEW_KEY_SUFFIX "([^"]+)"', text)
    block = re.search(
        r"vmaf_adm_extra_view_keys\[VMAF_ADM_VIEW_SCORE_COUNT\] = \{(.*?)\};", text, re.S
    )
    if not suffix or not block:
        raise AssertionError("adm_view_dist: suffix or vmaf_adm_extra_view_keys not found")
    bases = re.findall(r'"([^"]+)" VMAF_ADM_EXTRA_VIEW_KEY_SUFFIX', block.group(1))
    return [b + suffix.group(1) for b in bases]


def rust_extra_keys(text: str) -> list[str]:
    block = re.search(r"EXTRA_VIEW_NAMES: \[&CStr; 7\] = \[(.*?)\];", text, re.S)
    if not block:
        raise AssertionError("score.rs: EXTRA_VIEW_NAMES not found")
    return re.findall(r'c"([^"]+)"', block.group(1))


def option_entries(text: str, table_name: str = "options") -> list[str]:
    # C tables end in {0}, C++ ones (SYCL, in an anonymous namespace) in
    # {.name = nullptr}.
    table = re.search(
        rf"(?:static )?const VmafOption {table_name}\[\] = \{{(.*?)"
        rf"\{{(?:0|\.name = nullptr)\}}\}};",
        text,
        re.S,
    )
    if not table:
        raise AssertionError("integer_adm.c: option table not found")
    return re.split(r"\n    \{\n", table.group(1))[1:]


def non_feature_params(text: str, table_name: str = "options") -> set[str]:
    names = set()
    for entry in option_entries(text, table_name):
        name = re.search(r'\.name = "([^"]+)"', entry)
        if name and "VMAF_OPT_FLAG_FEATURE_PARAM" not in entry:
            names.add(name.group(1))
    return names


class AdmViewDistContract(unittest.TestCase):
    def test_c_and_rust_file_under_the_same_keys(self) -> None:
        c_keys = c_extra_keys(view_text())
        self.assertEqual(len(c_keys), 7)
        self.assertEqual(c_keys, rust_extra_keys(RUST_SCORE.read_text(encoding="utf-8")))

    def test_merge_handles_every_option_outside_the_names(self) -> None:
        for name, (path, table) in MERGING_TABLES.items():
            with self.subTest(extractor=name):
                text = path.read_text(encoding="utf-8")
                self.assertEqual(non_feature_params(text, table), HANDLED_NON_PARAMS)

    def test_planted_unflagged_option_is_caught(self) -> None:
        planted = c_text().replace(
            "    {0}};",
            '    {\n        .name = "adm_planted",\n        .type = VMAF_OPT_TYPE_BOOL,\n    },\n    {0}};',
            1,
        )
        self.assertIn("adm_planted", non_feature_params(planted))

    def test_planted_key_drift_is_caught(self) -> None:
        drifted = view_text().replace(
            '"integer_adm_scale3" VMAF_ADM_EXTRA_VIEW_KEY_SUFFIX', '"x"', 1
        )
        self.assertNotEqual(
            c_extra_keys(drifted), rust_extra_keys(RUST_SCORE.read_text(encoding="utf-8"))
        )


if __name__ == "__main__":
    sys.exit(unittest.main())
