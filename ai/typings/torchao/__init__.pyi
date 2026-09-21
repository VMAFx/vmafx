# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Typed subset of torchao 0.18 used by the QAT trainer (ADR-1281).

torchao ships no ``py.typed`` marker, so the repository owns a stub for the
narrow PT2E surface ``ai/train/qat.py`` calls. Keep it in step with the
``torchao>=0.18.0,<0.19`` floor declared in ``ai/pyproject.toml``.
"""
