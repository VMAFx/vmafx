# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""MkDocs hook: formulas are rendered only on pages that are not frozen (ADR-2705).

``pymdownx.arithmatex`` reads every ``$...$`` of every page. The Accepted ADR
bodies, ``docs/research/``, ``docs/changelog-archive/``, ``docs/state.md`` and
the rebase notes are frozen and were written before the syntax existed, so a
dollar sign there (a shell variable, a price) is not math. This hook puts the
dollar delimiters back around each element the extension produced on such a
page, so the page reads as written and ``scripts/docs/check_math.py`` has
nothing to compile there.
"""

from __future__ import annotations

import re
from typing import Any

FROZEN_PREFIXES = ("adr/", "research/", "changelog-archive/", "rebase-notes")
FROZEN_FILES = ("state.md",)
INLINE = re.compile(r'<span class="arithmatex">\\\((.*?)\\\)</span>', re.DOTALL)
BLOCK = re.compile(r'<div class="arithmatex">\\\[(.*?)\\\]</div>', re.DOTALL)


def is_frozen(src_path: str) -> bool:
    path = src_path.replace("\\", "/")
    return path in FROZEN_FILES or path.startswith(FROZEN_PREFIXES)


def restore_dollars(html: str) -> str:
    html = INLINE.sub(lambda m: "$" + m.group(1) + "$", html)
    return BLOCK.sub(lambda m: "$$" + m.group(1) + "$$", html)


def on_page_content(html: str, page: Any, **_kwargs: Any) -> str:
    if is_frozen(page.file.src_path):
        return restore_dollars(html)
    return html
