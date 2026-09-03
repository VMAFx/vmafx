# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Single source of truth for the default VMAF model used by ``vmaf-roi-score``.

See ``tools/vmaf-tune/src/vmaftune/defaultmodel.py`` for the full rationale.
The authoritative definition is ``VMAF_DEFAULT_MODEL_VERSION`` in
``core/include/libvmaf/model.h``; this is a mirror because the tool shells out
to the ``vmaf`` binary rather than linking libvmaf, and
``scripts/ci/check-default-model-single-source.sh`` fails the build if it
drifts from the header.

``vmaf-roi-score`` and ``vmaf-tune`` are separately installable distributions
with no shared runtime dependency, so each carries its own mirror rather than
one importing the other.
"""

from __future__ import annotations

#: Model version used when the caller names none. Must equal
#: ``VMAF_DEFAULT_MODEL_VERSION`` in ``core/include/libvmaf/model.h``.
DEFAULT_MODEL = "vmaf_v0.6.1"

__all__ = ["DEFAULT_MODEL"]
