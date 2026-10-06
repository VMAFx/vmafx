# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Values of the C macros an option names as its default (`default_macro`).

The definition never spells a library default (the default model is
VMAF_DEFAULT_MODEL_VERSION, ADR-1169); surfaces that cannot read a C header
(the MCP servers) need its value, so the generator reads the string macro from
the public headers at generation time and writes it into the generated file.
The drift check then fails when the header moves and the file was not
regenerated, which keeps the header the single source.
"""

from __future__ import annotations

import re
from pathlib import Path

from .model import Api, DefinitionError
from .options import all_options

HEADER_GLOB = "core/include/libvmaf/*.h"
DEFAULT_ROOT = Path(__file__).resolve().parents[3]


def _defines(root: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    pattern = re.compile(r'^#define\s+([A-Z][A-Z0-9_]*)\s+"([^"\\]*)"\s*$', re.MULTILINE)
    for header in sorted(root.glob(HEADER_GLOB)):
        for name, value in pattern.findall(header.read_text(encoding="utf-8")):
            out.setdefault(name, value)
    return out


def library_defaults(api: Api, root: Path | None = None) -> dict[str, str]:
    """Macro name -> string value of every `default_macro` of the option groups."""
    names = sorted({o.default_macro for _, o in all_options(api.option_groups) if o.default_macro})
    if not names:
        return {}
    defines = _defines(root or DEFAULT_ROOT)
    missing = [n for n in names if n not in defines]
    if missing:
        raise DefinitionError(f"default_macro not defined as a string in {HEADER_GLOB}: {missing}")
    return {name: defines[name] for name in names}
