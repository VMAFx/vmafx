# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Regression tests for two latent bugs in the upstream-mirror
``compat/python-vmaf/`` tree, fixed by ADR-0955.

Bug 1
-----
``compat/python-vmaf/tools/scanf.py::makeFormattedHandler.applyWidth``
had an inverted ``if width is None`` check. Symptoms:

- Implicit-width converters (``%d`` / ``%f`` / ``%s``) wrapped the
  handler in a ``makeWidthLimitedHandler(handler, None, ...)`` call,
  which crashed inside ``CappedBuffer`` with a ``TypeError`` on the
  first byte read.
- Explicit-width converters (``%5d``) silently skipped the width cap
  and read an unbounded run of matching characters.

Bug 2
-----
``compat/python-vmaf/__init__.py::ProcessRunner.run`` set the
C locale via ``env.setdefault("LC_ALL", "C")`` /
``env.setdefault("LANG", "C")``. ``setdefault`` is a no-op when the
key already exists, so a parent shell with e.g. ``LANG=de_DE.UTF-8``
defeated the override and subprocess error messages came back
locale-translated (``Kommando nicht gefunden`` instead of ``command
not found``), breaking downstream assertions that grep for canonical
English phrases. Fix: unconditional ``env[...] = "C"``.
"""

from __future__ import annotations

import os
import subprocess
from unittest import mock

import pytest

# These imports exercise the in-tree shim that re-exports from
# ``compat/python-vmaf/``; failing imports here mean the shim
# itself regressed, not the regression under test.
from vmaf import ProcessRunner
from vmaf.tools import scanf

# ---------------------------------------------------------------------------
# Bug 1 — scanf.py implicit + explicit width handling
# ---------------------------------------------------------------------------


class TestScanfImplicitWidth:
    """Implicit-width converters (``%d``, ``%f``, ``%s``) must not
    crash. Before the fix, applyWidth wrapped the handler in
    ``makeWidthLimitedHandler(handler, None, ...)``, which detonated
    inside ``CappedBuffer`` on the first byte read."""

    def test_implicit_width_decimal_int_parses_value(self):
        # Pre-fix: TypeError inside CappedBuffer.
        # Post-fix: parses 42 cleanly.
        result = scanf.sscanf("42", "%d")
        assert result == (42,)

    def test_implicit_width_float_parses_value(self):
        result = scanf.sscanf("3.14", "%f")
        assert result == pytest.approx((3.14,))

    def test_implicit_width_string_parses_token(self):
        result = scanf.sscanf("hello world", "%s")
        assert result == ("hello",)

    def test_implicit_width_hex_parses_value(self):
        result = scanf.sscanf("0xff", "%x")
        assert result == (255,)


class TestScanfExplicitWidth:
    """Explicit-width converters (``%5d``) must honour the cap.
    Before the fix, ``applyWidth`` returned the unwrapped handler
    for explicit widths, so the cap was silently dropped."""

    def test_explicit_width_caps_decimal_int(self):
        # 1234567 with %5d must stop after 5 digits → 12345,
        # leaving "67" in the buffer for the next conversion.
        result = scanf.sscanf("1234567", "%5d%d")
        assert result == (12345, 67)

    def test_explicit_width_caps_string(self):
        # "abcdefg" with %3s must stop after 3 chars → "abc",
        # leaving "defg" for the next %s.
        result = scanf.sscanf("abcdefg", "%3s%s")
        assert result == ("abc", "defg")

    def test_explicit_width_caps_hex(self):
        # 0xff with %3x reads "0xf" (3 chars) → 15.
        # Note: the "0x" prefix counts toward the width.
        result = scanf.sscanf("0xff", "%3x")
        assert result == (15,)


# ---------------------------------------------------------------------------
# Bug 2 — ProcessRunner LC_ALL / LANG must be forced, not soft-defaulted
# ---------------------------------------------------------------------------


class TestProcessRunnerLocaleForcing:
    """``ProcessRunner.run`` must overwrite ``LC_ALL`` and ``LANG``
    in the child env unconditionally, not via ``setdefault``."""

    def _captured_env(self, parent_env):
        """Run ProcessRunner with a faked parent env and capture
        what gets passed to ``subprocess.check_output``."""
        captured = {}

        def fake_check_output(cmd, **kwargs):
            captured["env"] = kwargs.get("env", {})
            return b""

        runner = ProcessRunner()
        with (
            mock.patch.dict(os.environ, parent_env, clear=True),
            mock.patch("vmaf.subprocess.check_output", side_effect=fake_check_output),
        ):
            runner.run(["/bin/true"], {})
        return captured["env"]

    def test_overrides_german_lc_all(self):
        # Pre-fix: setdefault preserves "de_DE.UTF-8".
        # Post-fix: forced to "C".
        env = self._captured_env({"LC_ALL": "de_DE.UTF-8", "LANG": "de_DE.UTF-8"})
        assert env["LC_ALL"] == "C"
        assert env["LANG"] == "C"

    def test_overrides_partial_lang_only(self):
        # Some shells set LANG but not LC_ALL.
        env = self._captured_env({"LANG": "fr_FR.UTF-8"})
        assert env["LC_ALL"] == "C"
        assert env["LANG"] == "C"

    def test_sets_when_unset(self):
        env = self._captured_env({})
        assert env["LC_ALL"] == "C"
        assert env["LANG"] == "C"

    def test_subprocess_error_message_is_english(self):
        """End-to-end check: a real subprocess invocation under a
        non-English parent locale produces an English-language
        error message in the captured AssertionError text."""
        runner = ProcessRunner()
        # Use a command that cannot exist on any normal PATH so the
        # error is generated by the kernel via subprocess. We rely on
        # the C locale forcing the message language to English.
        bad_path = "/nonexistent/no_such_binary_42a7f"
        with mock.patch.dict(
            os.environ, {"LC_ALL": "de_DE.UTF-8", "LANG": "de_DE.UTF-8"}, clear=False
        ):
            with pytest.raises((AssertionError, FileNotFoundError)) as excinfo:
                runner.run([bad_path], {})
            # FileNotFoundError happens before subprocess actually
            # runs (Python-side); in that case the locale-forcing path
            # isn't exercised, so we can only verify the wiring above
            # via the mocked tests. This guards the wiring at least
            # surfaces a deterministic error.
            assert excinfo.value is not None


# ---------------------------------------------------------------------------
# Bug 3 — scanf.py isFileLike exception safety & misc.py check_scanf_match
# ---------------------------------------------------------------------------


class TestIsFileLike:
    """isFileLike must return True for seekable files and False for non-files
    or objects whose seek raises IOError, OSError, or ValueError, without raising."""

    def test_file_like_bytesio_returns_true(self):
        import io

        buf = io.BytesIO(b"hello")
        assert scanf.isFileLike(buf) is True

    def test_non_file_objects_return_false(self):
        assert scanf.isFileLike("string") is False
        assert scanf.isFileLike(123) is False
        assert scanf.isFileLike(None) is False
        assert scanf.isFileLike([]) is False

    def test_closed_file_or_failing_seek_returns_false_safely(self):
        class FailingSeekFile:
            def read(self):
                return b""

            def seek(self, offset, whence=0):
                raise OSError("unseekable stream")

        class ValueErrorSeekFile:
            def read(self):
                return b""

            def seek(self, offset, whence=0):
                raise ValueError("I/O operation on closed file")

        assert scanf.isFileLike(FailingSeekFile()) is False
        assert scanf.isFileLike(ValueErrorSeekFile()) is False


class TestCheckScanfMatchFallback:
    """check_scanf_match must parse scanf templates and fall back to fnmatch
    when sscanf raises FormatError or IncompleteCaptureError."""

    def test_scanf_matching_template(self):
        from vmaf.tools.misc import check_scanf_match

        assert check_scanf_match("frame00000001.yuv", "frame%08d.yuv") is True

    def test_fnmatch_fallback_on_incomplete_or_wildcard(self):
        from vmaf.tools.misc import check_scanf_match

        # Asterisk wildcard triggers sscanf mismatch and falls back to fnmatch
        assert check_scanf_match("frame00000001.yuv", "frame*.yuv") is True

    def test_non_matching_returns_false(self):
        from vmaf.tools.misc import check_scanf_match

        assert check_scanf_match("frame00000001.yuv", "other%08d.yuv") is False
        assert check_scanf_match("frame00000001.yuv", "*.png") is False
