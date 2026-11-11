#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Environment isolation for scanner checks run inside fixture repositories."""

from __future__ import annotations

import os
import unittest
from unittest import mock


def isolate_pre_commit_environment(test_case: unittest.TestCase) -> None:
    """Remove caller hook variables until the test's registered cleanups run."""
    test_case.enterContext(mock.patch.dict(os.environ))
    for key in tuple(os.environ):
        if key.startswith("PRE_COMMIT_"):
            del os.environ[key]
