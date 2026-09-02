# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""``vmaf-dev-llm`` — local LLM dev helpers for the Lusoris VMAF fork.

Powers the ``/dev-llm-*`` skills (commit-msg drafting, Doxygen
generation, code review, model-card drafting). Sub-modules:

* :mod:`vmaf_dev_llm.cli`              — Click-based CLI entry point.
* :mod:`vmaf_dev_llm.config`           — config-file resolution helpers.
* :mod:`vmaf_dev_llm.ollama_client`    — minimal Ollama HTTP client wrapper.
* :mod:`vmaf_dev_llm.modelcard_facts`  — fact collector for the
  ``/dev-llm-modelcard`` skill.

Per ADR-0911 the package surface is a namespace — there are no
re-exported symbols at package level.
"""

__version__ = "3.2.1"  # x-release-please-version

__all__ = ["__version__"]
