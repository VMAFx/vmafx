# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Single source of truth for the default VMAF model used by ``vmaf-mcp``.

The authoritative definition lives in C, as ``VMAF_DEFAULT_MODEL_VERSION`` in
``core/include/libvmaf/model.h``. This server shells out to the ``vmaf`` binary
rather than linking the library, so it carries the mirror below.

The mirror is kept honest mechanically:
``scripts/ci/check-default-model-single-source.sh`` parses the C header and
fails the build if this constant disagrees with it, and also fails if any
module reintroduces its own hardcoded default.
"""

from __future__ import annotations

#: Model version used when the caller names none. Must equal
#: ``VMAF_DEFAULT_MODEL_VERSION`` in ``core/include/libvmaf/model.h``.
DEFAULT_MODEL = "vmaf_v1.0.16_3d0h"

#: The default as a ``--model`` argument (``version=<name>``).
DEFAULT_MODEL_ARG = f"version={DEFAULT_MODEL}"

__all__ = ["DEFAULT_MODEL", "DEFAULT_MODEL_ARG"]
