#!/usr/bin/env python3
"""Write the installed Cppcheck POSIX model with verified VMAFx corrections."""

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

FUNCTION_PATTERN = re.compile(rb"<function\b[^>]*\bname\s*=\s*['\"]pthread_cond_init['\"][^>]*>")
ARGUMENT_PATTERNS = {
    number: re.compile(rb"<arg\b[^>]*\bnr\s*=\s*['\"]" + number + rb"['\"][^>]*>")
    for number in (b"1", b"2")
}
NOT_NULL_PATTERN = re.compile(rb"<not-null\s*/>")
CONDITION_MODEL = b"""  <function name="pthread_cond_init">
    <returnValue type="int"/>
    <noreturn>false</noreturn>
    <leak-ignore/>
    <arg nr="1" direction="out"><not-null/></arg>
    <arg nr="2" direction="in"/>
  </function>"""


def legacy_filesdir(binary: str) -> Path | None:
    """Find models beside pre-2.18 analyzers that lack ``--filesdir``."""
    resolved = shutil.which(binary)
    if resolved is None and Path(binary).is_absolute():
        resolved = binary
    if resolved is None:
        return None
    executable = Path(resolved).resolve()
    candidates = (
        executable.parent,
        executable.parent.parent / "share/cppcheck",
    )
    for candidate in candidates:
        if (candidate / "cfg/posix.cfg").is_file():
            return candidate
    return None


def cppcheck_filesdir(binary: str) -> Path:
    """Return Cppcheck's compiled-in data directory or fail closed."""
    try:
        result = subprocess.run(  # noqa: S603 -- caller-selected analyzer binary
            [binary, "--filesdir"], capture_output=True, text=True, check=False
        )
    except OSError as error:
        raise ValueError(f"cannot execute Cppcheck binary {binary}: {error}") from error
    filesdir = result.stdout.strip()
    if result.returncode == 0 and filesdir and "\n" not in filesdir:
        root = Path(filesdir)
        if (root / "cfg/posix.cfg").is_file():
            return root
        raise ValueError(f"Cppcheck filesdir has no POSIX model: {root}")
    fallback = legacy_filesdir(binary)
    if fallback is None:
        detail = (result.stdout + result.stderr).strip()
        raise ValueError(f"cannot resolve Cppcheck filesdir: {detail or result.returncode}")
    return fallback


def argument_span(function: bytes, number: bytes) -> tuple[int, int, bool]:
    """Return one argument's byte span and whether it is self-closing."""
    matches = list(ARGUMENT_PATTERNS[number].finditer(function))
    if len(matches) != 1:
        raise ValueError(
            f"expected one pthread_cond_init argument {number.decode()}, found {len(matches)}"
        )
    match = matches[0]
    if match.group().rstrip().endswith(b"/>"):
        return match.start(), match.end(), True
    end = function.find(b"</arg>", match.end())
    if end < 0:
        raise ValueError(f"unterminated pthread_cond_init argument {number.decode()}")
    return match.start(), end + len(b"</arg>"), False


def remove_marker_line(function: bytes, marker_start: int, marker_end: int) -> bytes:
    """Remove a standalone marker line cleanly, or only its inline tag."""
    line_start = function.rfind(b"\n", 0, marker_start) + 1
    next_newline = function.find(b"\n", marker_end)
    line_end = len(function) if next_newline < 0 else next_newline + 1
    before = function[line_start:marker_start]
    after_end = len(function) if next_newline < 0 else next_newline
    after = function[marker_end:after_end]
    if not before.strip() and not after.strip():
        return function[:line_start] + function[line_end:]
    return function[:marker_start] + function[marker_end:]


def insert_condition_model(model: bytes) -> bytes:
    """Add the missing correct contract used by pre-2.22 Cppcheck models."""
    closing = list(re.finditer(rb"</def\s*>", model))
    if len(closing) != 1:
        raise ValueError(f"expected one POSIX model closing tag, found {len(closing)}")
    newline = b"\r\n" if b"\r\n" in model else b"\n"
    fragment = CONDITION_MODEL.replace(b"\n", newline)
    position = closing[0].start()
    prefix = model[:position]
    separator = b"" if prefix.endswith((b"\n", b"\r")) else newline
    return prefix + separator + fragment + newline + model[position:]


def corrected_model(source: Path) -> bytes:
    """Return the installed model with the exact pthread contract corrected."""
    try:
        model = source.read_bytes()
    except OSError as error:
        raise ValueError(f"cannot read installed POSIX model {source}: {error}") from error
    functions = list(FUNCTION_PATTERN.finditer(model))
    if not functions:
        return insert_condition_model(model)
    if len(functions) != 1:
        raise ValueError(f"expected at most one pthread_cond_init model, found {len(functions)}")
    start = functions[0].start()
    end = model.find(b"</function>", functions[0].end())
    if end < 0:
        raise ValueError("unterminated pthread_cond_init model")
    end += len(b"</function>")
    function = model[start:end]
    first_start, first_end, first_self_closing = argument_span(function, b"1")
    second_start, second_end, second_self_closing = argument_span(function, b"2")
    first_markers = list(NOT_NULL_PATTERN.finditer(function[first_start:first_end]))
    if first_self_closing or len(first_markers) != 1:
        raise ValueError("pthread_cond_init argument 1 must keep exactly one non-null marker")
    if second_self_closing:
        return model
    second = function[second_start:second_end]
    markers = list(NOT_NULL_PATTERN.finditer(second))
    if not markers:
        return model
    if len(markers) != 1:
        raise ValueError("pthread_cond_init argument 2 has multiple non-null markers")
    marker = markers[0]
    marker_start = second_start + marker.start()
    marker_end = second_start + marker.end()
    corrected_function = remove_marker_line(function, marker_start, marker_end)
    return model[:start] + corrected_function + model[end:]


def validate_model(binary: str, model: Path, directory: Path) -> None:
    """Ask the selected Cppcheck binary to parse the generated model."""
    descriptor, control_name = tempfile.mkstemp(
        prefix=".cppcheck-model-control.", suffix=".c", dir=directory
    )
    os.close(descriptor)
    control = Path(control_name)
    try:
        result = subprocess.run(  # noqa: S603 -- caller-selected analyzer binary
            [
                binary,
                "--check-library",
                f"--library={model}",
                "--error-exitcode=1",
                str(control),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as error:
        raise ValueError(f"cannot validate generated POSIX model: {error}") from error
    finally:
        control.unlink(missing_ok=True)
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise ValueError(f"generated POSIX model failed Cppcheck validation: {detail}")


def write_model(binary: str, output: Path) -> None:
    """Derive and atomically publish the corrected model."""
    source = cppcheck_filesdir(binary) / "cfg" / "posix.cfg"
    model = corrected_model(source)
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{output.name}.", dir=output.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(model)
            stream.flush()
            os.fsync(stream.fileno())
        validate_model(binary, temporary, output.parent)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cppcheck", default="cppcheck")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        write_model(args.cppcheck, args.output.resolve())
    except ValueError as error:
        parser.exit(1, f"write-cppcheck-posix-model: error: {error}\n")
    print(f"write-cppcheck-posix-model: wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
