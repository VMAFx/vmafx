# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Single source of truth for the default VMAF model used by ``vmaf-tune``.

The authoritative definition lives in C, as ``VMAF_DEFAULT_MODEL_VERSION`` in
``core/include/libvmaf/model.h``; anything linking libvmaf reads it at runtime
through ``vmaf_default_model_version()``. ``vmaf-tune`` is a pure-Python tool
that shells out to the ``vmaf`` binary rather than linking the library, so it
carries the mirror below instead of growing a C extension just to learn a
string.

The mirror is kept honest mechanically rather than by discipline:
``scripts/ci/check-default-model-single-source.sh`` parses the C header and
fails the build if this constant disagrees with it, and also fails if any
module reintroduces its own hardcoded default. Changing the fork's default
model is a one-line edit to the C header plus whatever the gate then reports.

Import ``DEFAULT_MODEL`` here rather than writing the version string inline.
"""

from __future__ import annotations

#: Model version used when the caller names none. Must equal
#: ``VMAF_DEFAULT_MODEL_VERSION`` in ``core/include/libvmaf/model.h``.
DEFAULT_MODEL = "vmaf_v0.6.1"

#: No-Enhancement-Gain companion to :data:`DEFAULT_MODEL`, used when a caller
#: asks for NEG scoring without naming a model. Every NEG model in the tree is
#: its base name with ``neg`` appended.
DEFAULT_MODEL_NEG = DEFAULT_MODEL + "neg"

__all__ = ["DEFAULT_MODEL", "DEFAULT_MODEL_NEG"]
