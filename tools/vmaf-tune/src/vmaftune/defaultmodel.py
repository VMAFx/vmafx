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
DEFAULT_MODEL = "vmaf_v1.0.16_3d0h"

#: Model used when a caller asks for NEG scoring without naming a model.
#:
#: Deliberately NOT derived from :data:`DEFAULT_MODEL`. No-Enhancement-Gain is a
#: v0.6.1-family concept: Netflix published NEG variants for ``vmaf_v0.6.1``,
#: ``vmaf_float_v0.6.1`` and ``vmaf_4k_v0.6.1`` only, and there is no NEG
#: counterpart to any ``vmaf_v1.0.16_*`` model. ``DEFAULT_MODEL + "neg"`` would
#: synthesise ``vmaf_v1.0.16_3d0hneg``, which does not exist and which libvmaf
#: rejects at load time. Until Netflix publishes a v1 NEG model, NEG scoring
#: stays on the v0.6.1 family and the two constants name different generations.
DEFAULT_MODEL_NEG = "vmaf_v0.6.1neg"

__all__ = ["DEFAULT_MODEL", "DEFAULT_MODEL_NEG"]
