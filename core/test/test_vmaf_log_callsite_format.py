#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Lock the user-visible formatting of restored libvmaf diagnostics."""

from __future__ import annotations

import ast
import unittest
from pathlib import Path

SOURCE_ROOT = Path(__file__).resolve().parents[1] / "src"
Token = tuple[str, str]
LogCall = tuple[str, str | None]
MIN_LOG_ARGUMENTS = 2


def _quoted_literal_end(source: str, start: int) -> int:
    quote = source[start]
    index = start + 1
    while index < len(source):
        if source[index] == "\\":
            index += 2
        elif source[index] == quote:
            return index + 1
        else:
            index += 1
    raise ValueError("unterminated C string or character literal")


def _tokenize_c(source: str) -> list[Token]:
    """Tokenize enough C/C++ to inspect ordinary function-call arguments."""
    tokens: list[Token] = []
    index = 0
    while index < len(source):
        char = source[index]
        if char.isspace():
            index += 1
            continue
        if source.startswith("//", index):
            newline = source.find("\n", index + 2)
            index = len(source) if newline < 0 else newline + 1
            continue
        if source.startswith("/*", index):
            comment_end = source.find("*/", index + 2)
            if comment_end < 0:
                raise ValueError("unterminated C block comment")
            index = comment_end + 2
            continue
        if char in {'"', "'"}:
            start = index
            index = _quoted_literal_end(source, start)
            tokens.append(("string" if char == '"' else "character", source[start:index]))
            continue
        if char.isalpha() or char == "_":
            start = index
            index += 1
            while index < len(source) and (source[index].isalnum() or source[index] == "_"):
                index += 1
            tokens.append(("identifier", source[start:index]))
            continue
        tokens.append(("punctuation", char))
        index += 1
    return tokens


def _call_arguments(tokens: list[Token], open_index: int) -> tuple[list[list[Token]], int]:
    arguments: list[list[Token]] = []
    current: list[Token] = []
    delimiters: list[str] = []
    closing = {"(": ")", "[": "]", "{": "}"}
    for index in range(open_index + 1, len(tokens)):
        kind, value = tokens[index]
        if kind == "punctuation" and value in closing:
            delimiters.append(closing[value])
        elif kind == "punctuation" and value in closing.values():
            if not delimiters:
                if value != ")":
                    raise ValueError("mismatched delimiter in vmaf_log call")
                arguments.append(current)
                return arguments, index
            if delimiters.pop() != value:
                raise ValueError("mismatched delimiter in vmaf_log argument")
        elif kind == "punctuation" and value == "," and not delimiters:
            arguments.append(current)
            current = []
            continue
        current.append((kind, value))
    raise ValueError("unterminated vmaf_log call")


def _decode_string_argument(tokens: list[Token]) -> str | None:
    if not tokens or any(kind != "string" for kind, _ in tokens):
        return None
    parts: list[str] = []
    for _, spelling in tokens:
        try:
            value = ast.literal_eval(spelling)
        except (SyntaxError, ValueError):
            return None
        if not isinstance(value, str):
            return None
        parts.append(value)
    return "".join(parts)


def extract_vmaf_log_calls(source: str) -> list[LogCall]:
    """Return severity and evaluated ordinary-string message for each logical call."""
    tokens = _tokenize_c(source)
    calls: list[LogCall] = []
    index = 0
    while index + 1 < len(tokens):
        if tokens[index] != ("identifier", "vmaf_log") or tokens[index + 1] != (
            "punctuation",
            "(",
        ):
            index += 1
            continue
        arguments, end_index = _call_arguments(tokens, index + 1)
        if len(arguments) >= MIN_LOG_ARGUMENTS:
            level = "".join(spelling for _, spelling in arguments[0])
            calls.append((level, _decode_string_argument(arguments[1])))
        index = end_index + 1
    return calls


def _diagnostic_key(message: str) -> str:
    if message.startswith("Error: "):
        message = message.removeprefix("Error: ")
    return message.rstrip("\n")


def validate_log_contract(source: str, expected: dict[str, int]) -> None:
    """Require exact ERROR-level, newline-terminated calls for expected diagnostics."""
    calls = extract_vmaf_log_calls(source)
    for message_body, expected_count in expected.items():
        matches = [
            (level, message)
            for level, message in calls
            if message is not None and _diagnostic_key(message) == message_body
        ]
        if len(matches) != expected_count:
            raise AssertionError(
                f"{message_body!r}: found {len(matches)} logical calls, expected {expected_count}"
            )
        for level, message in matches:
            if level != "VMAF_LOG_LEVEL_ERROR":
                raise AssertionError(f"{message_body!r}: expected ERROR level, found {level!r}")
            if message != f"{message_body}\n":
                raise AssertionError(
                    f"{message_body!r}: expected one newline and no redundant prefix, found {message!r}"
                )


class VmafLogCallsiteFormatTests(unittest.TestCase):
    def source(self, relative: str) -> str:
        return (SOURCE_ROOT / relative).read_text(encoding="utf-8")

    def test_cuda_error_level_does_not_repeat_severity(self) -> None:
        source = self.source("cuda/common.c")
        validate_log_contract(
            source,
            {
                "failed to initialize CUDA": 2,
                "device_id %d is out of range": 1,
            },
        )

    def test_restored_diagnostics_are_line_terminated(self) -> None:
        expected = {
            "feature/luminance_tools.cpp": {
                "unknown pixel range received": 1,
                "unknown EOTF received": 1,
            },
            "feature/speed.c": {
                "SpEED: image too small, operating width or height is 0": 1,
                "invalid speed_kernelscale": 1,
            },
            "feature/vif.c": {"invalid vif_kernelscale: %f": 1},
        }

        for relative, messages in expected.items():
            with self.subTest(source=relative):
                validate_log_contract(self.source(relative), messages)

    def test_logical_call_parser_red_caps(self) -> None:
        bad_sources = {
            "comment bait": (
                r"""/* vmaf_log(VMAF_LOG_LEVEL_ERROR, "unknown EOTF received\n"); */
vmaf_log(VMAF_LOG_LEVEL_ERROR, "unknown EOTF received");""",
                {"unknown EOTF received": 1},
                "expected one newline",
            ),
            "missing newline": (
                r"""vmaf_log(VMAF_LOG_LEVEL_ERROR, "invalid speed_kernelscale");""",
                {"invalid speed_kernelscale": 1},
                "expected one newline",
            ),
            "wrong severity": (
                r"""vmaf_log(VMAF_LOG_LEVEL_WARNING, "invalid vif_kernelscale: %f\n", scale);""",
                {"invalid vif_kernelscale: %f": 1},
                "expected ERROR level",
            ),
            "split prefix": (
                r"""vmaf_log(
    VMAF_LOG_LEVEL_ERROR,
    "Error: " "failed to initialize CUDA\n");""",
                {"failed to initialize CUDA": 1},
                "expected one newline and no redundant prefix",
            ),
        }

        for name, (source, expected, error_pattern) in bad_sources.items():
            with self.subTest(red_cap=name):
                with self.assertRaisesRegex(AssertionError, error_pattern):
                    validate_log_contract(source, expected)

        adjacent = extract_vmaf_log_calls(r"""vmaf_log(VMAF_LOG_LEVEL_ERROR, "failed to " /* gap */
                         "initialize CUDA\n");""")
        self.assertEqual(adjacent, [("VMAF_LOG_LEVEL_ERROR", "failed to initialize CUDA\n")])


if __name__ == "__main__":
    unittest.main()
