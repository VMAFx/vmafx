#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Keep HIP integer-ADM's large kernel argument device-resident (ADR-0759)."""

from __future__ import annotations

import re
import unittest
from collections.abc import Callable
from pathlib import Path

FEATURE_DIR = Path(__file__).resolve().parents[1] / "src" / "feature" / "hip"
SOURCE_PATHS = {
    "host": "integer_adm_hip.c",
    "csf": "integer_adm/adm_csf.hip",
    "cm": "integer_adm/adm_cm.hip",
}
LAUNCHERS = {
    "adm_csf_device_hip": "s->func_adm_csf_kernel_1_4",
    "i4_adm_csf_device_hip": "s->func_i4_adm_csf_kernel_1_4",
    "i4_adm_cm_device_hip": "s->func_i4_adm_cm_line_kernel",
    "adm_cm_device_hip": "s->func_adm_cm_line_kernel_8",
}


def _strip_comments(source: str) -> str:
    """Remove C comments without treating comment markers in literals as syntax."""
    output: list[str] = []
    index = 0
    quote = ""
    while index < len(source):
        if quote:
            char = source[index]
            output.append(char)
            if char == "\\" and index + 1 < len(source):
                index += 1
                output.append(source[index])
            elif char == quote:
                quote = ""
            index += 1
            continue
        if source[index] in {'"', "'"}:
            quote = source[index]
            output.append(source[index])
            index += 1
            continue
        if source.startswith("//", index):
            newline = source.find("\n", index + 2)
            index = len(source) if newline < 0 else newline
            continue
        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            if end < 0:
                raise ValueError("unterminated C block comment")
            output.append(" ")
            output.extend("\n" for char in source[index : end + 2] if char == "\n")
            index = end + 2
            continue
        output.append(source[index])
        index += 1
    return "".join(output)


def _normalize(source: str) -> str:
    return re.sub(r"\s+", " ", _strip_comments(source).replace("\\\n", " ")).strip()


def _compact(source: str) -> str:
    return re.sub(r"\s+", "", _strip_comments(source).replace("\\\n", ""))


def _matching_delimiter(source: str, start: int, opening: str, closing: str) -> int:
    depth = 0
    quote = ""
    index = start
    while index < len(source):
        char = source[index]
        if quote:
            if char == "\\":
                index += 2
                continue
            if char == quote:
                quote = ""
        elif char in {'"', "'"}:
            quote = char
        elif char == opening:
            depth += 1
        elif char == closing:
            depth -= 1
            if depth == 0:
                return index
        index += 1
    raise ValueError(f"unterminated {opening}{closing} region")


def _function(source: str, name: str) -> str:
    """Return a named C/C++ function definition, excluding calls and declarations."""
    clean = _strip_comments(source)
    for match in re.finditer(rf"\b{re.escape(name)}\s*\(", clean):
        open_paren = clean.find("(", match.start())
        close_paren = _matching_delimiter(clean, open_paren, "(", ")")
        open_brace = close_paren + 1
        while open_brace < len(clean) and clean[open_brace].isspace():
            open_brace += 1
        if open_brace >= len(clean) or clean[open_brace] != "{":
            continue
        close_brace = _matching_delimiter(clean, open_brace, "{", "}")
        return clean[match.start() : close_brace + 1]
    raise ValueError(f"missing function definition: {name}")


def _macro(source: str, name: str) -> str:
    lines = _strip_comments(source).splitlines()
    for line_index, line in enumerate(lines):
        if re.match(rf"\s*#define\s+{re.escape(name)}\b", line):
            body = [line]
            cursor = line_index
            while body[-1].rstrip().endswith("\\"):
                cursor += 1
                body.append(lines[cursor])
            return _normalize("\n".join(body))
    raise ValueError(f"missing macro definition: {name}")


def _first_parameter(source: str, callable_name: str) -> str:
    clean = _strip_comments(source).replace("\\\n", " ")
    match = re.search(rf"\b{re.escape(callable_name)}\s*\(", clean)
    if match is None:
        raise ValueError(f"missing callable signature: {callable_name}")
    open_paren = clean.find("(", match.start())
    close_paren = _matching_delimiter(clean, open_paren, "(", ")")
    parameters = clean[open_paren + 1 : close_paren]
    return _normalize(parameters.split(",", 1)[0])


def _block_after(source: str, pattern: str, start: int = 0) -> str:
    clean = _strip_comments(source)
    match = re.search(pattern, clean[start:])
    if match is None:
        raise ValueError(f"missing braced region: {pattern}")
    match_end = start + match.end()
    open_brace = clean.find("{", match_end - 1)
    if open_brace < 0:
        raise ValueError(f"missing opening brace after: {pattern}")
    close_brace = _matching_delimiter(clean, open_brace, "{", "}")
    return clean[open_brace + 1 : close_brace]


def _sub_exact(source: str, pattern: str, replacement: str, count: int = 1) -> str:
    mutated, replacements = re.subn(pattern, replacement, source, count=count)
    if replacements != count:
        raise AssertionError(
            f"mutation expected {count} replacement(s), got {replacements}: {pattern}"
        )
    return mutated


def _sources(feature_dir: Path = FEATURE_DIR) -> dict[str, str]:
    return {
        role: (feature_dir / relative).read_text(encoding="utf-8")
        for role, relative in SOURCE_PATHS.items()
    }


def _require_order(failures: list[str], scope: str, source: str, markers: tuple[str, ...]) -> None:
    compact = _compact(source)
    cursor = 0
    for marker in markers:
        needle = _compact(marker)
        found = compact.find(needle, cursor)
        if found < 0:
            failures.append(f"{scope}: missing or reordered {marker}")
            return
        cursor = found + len(needle)


def _validate_kernel_signatures(sources: dict[str, str], failures: list[str]) -> None:
    clean_kernels = _strip_comments(sources["csf"] + "\n" + sources["cm"])
    if re.search(r"\bAdmBufferHip\b(?!\s*[*&])\s+[A-Za-z_]\w*", clean_kernels):
        failures.append("kernels: AdmBufferHip must never be passed by value")

    macro_kernels = {
        "ADM_CSF_KERNEL": "adm_csf_kernel_##rows_per_thread##_##cols_per_thread",
        "I4_ADM_CSF_KERNEL": "i4_adm_csf_kernel_##rows_per_thread##_##cols_per_thread",
    }
    pointer_pattern = r"const\s+AdmBufferHip\s*\*\s*__restrict__\s+\w+"
    for macro_name, kernel_name in macro_kernels.items():
        macro = _macro(sources["csf"], macro_name)
        first_parameter = _first_parameter(macro, kernel_name)
        if re.fullmatch(pointer_pattern, first_parameter) is None:
            failures.append(f"{macro_name}: first argument is not the device buffer pointer")

    for kernel in ("i4_adm_cm_line_kernel", "adm_cm_line_kernel_8"):
        first_parameter = _first_parameter(_function(sources["cm"], kernel), kernel)
        if re.fullmatch(pointer_pattern, first_parameter) is None:
            failures.append(f"{kernel}: first argument is not the device buffer pointer")

    reduce_signature = _normalize(
        _function(sources["cm"], "adm_cm_reduce_line_kernel_4").split("{", 1)[0]
    )
    if "AdmBufferHip" in reduce_signature:
        failures.append("adm_cm_reduce_line_kernel_4: buffer-free reduce gained AdmBufferHip")


def _validate_launches(host: str, failures: list[str]) -> None:
    for launcher, kernel_handle in LAUNCHERS.items():
        body = _function(host, launcher)
        launch_match = re.search(re.escape(kernel_handle), body)
        arrays = list(re.finditer(r"\bvoid\s*\*\s*args\s*\[\s*\]\s*=\s*\{", body))
        arrays = [
            match for match in arrays if launch_match and match.start() < launch_match.start()
        ]
        if not arrays:
            failures.append(f"{launcher}: first kernel argument is not &s->buf_dev")
            continue
        open_brace = body.find("{", arrays[-1].start())
        close_brace = _matching_delimiter(body, open_brace, "{", "}")
        initializer = body[open_brace + 1 : close_brace]
        first_argument = initializer.split(",", 1)[0]
        pointer_pattern = r"(?:\(\s*void\s*\*\s*\)\s*)?&\s*s\s*->\s*buf_dev"
        if re.fullmatch(pointer_pattern, first_argument.strip()) is None:
            failures.append(f"{launcher}: first kernel argument is not &s->buf_dev")
        if len(re.findall(r"&\s*s\s*->\s*buf_dev\b", initializer)) != 1:
            failures.append(f"{launcher}: expected exactly one device-buffer argument")


def _validate_upload(host: str, failures: list[str]) -> None:
    upload = _function(host, "adm_hip_upload_buf")
    _require_order(
        failures,
        "adm_hip_upload_buf",
        upload,
        (
            "void *dev = NULL;",
            "hipMalloc(&dev, sizeof(s->buf))",
            "hipMemcpy(dev, &s->buf, sizeof(s->buf), hipMemcpyHostToDevice)",
            "hipFree(dev)",
            "s->buf_dev = dev;",
        ),
    )
    if "hipMalloc((void**)&s->buf_dev" in _compact(upload):
        failures.append("adm_hip_upload_buf: publishes the allocation before the copy succeeds")

    copy_match = re.search(r"hipMemcpy\s*\([^;]+hipMemcpyHostToDevice\s*\)\s*;", upload)
    if copy_match is None:
        failures.append("adm_hip_upload_buf: missing host-to-device copy")
    else:
        copy_failure = _block_after(
            upload,
            r"if\s*\(\s*hip_err\s*!=\s*hipSuccess\s*\)\s*\{",
            copy_match.end(),
        )
        _require_order(
            failures,
            "adm_hip_upload_buf copy failure",
            copy_failure,
            ("hipFree(dev);", "return hip_rc(hip_err);"),
        )
        if "s->buf_dev" in copy_failure:
            failures.append("adm_hip_upload_buf: publishes the allocation on copy failure")

    free_buf = _function(host, "adm_hip_free_buf_dev")
    _require_order(
        failures,
        "adm_hip_free_buf_dev",
        free_buf,
        ("s->buf_dev != NULL", "hipFree(s->buf_dev)", "s->buf_dev = NULL;"),
    )


def _validate_lifecycle(host: str, failures: list[str]) -> None:
    init_device = _function(host, "adm_hip_init_device")
    _require_order(
        failures,
        "adm_hip_init_device upload",
        init_device,
        ("adm_hip_slice_bands(s, h);", "adm_hip_slice_results(s);", "adm_hip_upload_buf(s);"),
    )
    upload_match = re.search(r"adm_hip_upload_buf\s*\(\s*s\s*\)\s*;", init_device)
    if upload_match is None:
        raise ValueError("adm_hip_init_device: missing upload call")
    upload_failure = _block_after(init_device, r"if\s*\(\s*err\s*\)\s*\{", upload_match.end())
    _require_order(
        failures,
        "adm_hip_init_device upload failure",
        upload_failure,
        (
            "adm_hip_free_luma(s);",
            "adm_hip_free_buffers(s);",
            "adm_hip_unload_modules(s);",
            "adm_hip_destroy_stream(s);",
        ),
    )

    init_fex = _function(host, "init_fex_hip")
    dictionary_failure = _block_after(
        init_fex,
        r"if\s*\(\s*s\s*->\s*feature_name_dict\s*==\s*NULL\s*\)\s*\{",
    )
    _require_order(
        failures,
        "init_fex_hip dictionary failure",
        dictionary_failure,
        (
            "adm_hip_free_buf_dev(s);",
            "adm_hip_free_luma(s);",
            "adm_hip_free_buffers(s);",
            "adm_hip_unload_modules(s);",
            "adm_hip_destroy_stream(s);",
            "return -ENOMEM;",
        ),
    )

    close = _function(host, "close_fex_hip")
    _require_order(
        failures,
        "close_fex_hip",
        close,
        (
            "adm_hip_close_stream(s);",
            "adm_hip_unload_modules(s);",
            "adm_hip_free_buf_dev(s);",
            "adm_hip_free_luma(s);",
            "adm_hip_free_buffers(s);",
        ),
    )


def _contract_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    host = sources["host"]
    state = _block_after(host, r"typedef\s+struct\s+AdmStateHip\s*\{")
    if len(re.findall(r"\bAdmBufferHip\s*\*\s*buf_dev\s*;", state)) != 1:
        failures.append("AdmStateHip: expected one device-resident AdmBufferHip owner")
    validators: tuple[Callable[[], None], ...] = (
        lambda: _validate_kernel_signatures(sources, failures),
        lambda: _validate_launches(host, failures),
        lambda: _validate_upload(host, failures),
        lambda: _validate_lifecycle(host, failures),
    )
    for validate in validators:
        try:
            validate()
        except ValueError as error:
            failures.append(str(error))
    return failures


class HipAdmBufferPointerContractTest(unittest.TestCase):
    def test_live_sources_keep_pointer_and_lifecycle_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_by_value_kernel_regression_is_detected(self) -> None:
        sources = _sources()
        sources["cm"] = _sub_exact(
            sources["cm"],
            r"(__global__\s+void\s+i4_adm_cm_line_kernel\s*\(\s*)"
            r"const\s+AdmBufferHip\s*\*\s*__restrict__\s+buf_ptr",
            r"\1AdmBufferHip buffer",
        )
        self.assertTrue(any("passed by value" in item for item in _contract_failures(sources)))

    def test_benign_declaration_whitespace_is_accepted(self) -> None:
        sources = _sources()
        sources["csf"] = _sub_exact(
            sources["csf"],
            r"\(\s*\\\n\s*const AdmBufferHip",
            "(const AdmBufferHip",
            count=2,
        )
        sources["cm"] = _sub_exact(
            sources["cm"],
            r"(i4_adm_cm_line_kernel|adm_cm_line_kernel_8)\(\s*const AdmBufferHip",
            r"\1(\n    const AdmBufferHip",
            count=2,
        )
        sources["host"] = _sub_exact(
            sources["host"],
            r"void\s*\*\s*args\s*\[\s*\]\s*=\s*\{\s*"
            r"\(\s*void\s*\*\s*\)\s*&\s*s\s*->\s*buf_dev\s*,",
            "void *args[] = { (void*) &s->buf_dev,",
            count=4,
        )
        sources["host"] = _sub_exact(
            sources["host"],
            r"AdmBufferHip\s*\*\s*buf_dev\s*;",
            "AdmBufferHip* buf_dev;",
        )
        self.assertEqual(_contract_failures(sources), [])

    def test_host_launch_regression_is_detected(self) -> None:
        sources = _sources()
        sources["host"] = _sub_exact(
            sources["host"],
            r"\(\s*void\s*\*\s*\)\s*&\s*s\s*->\s*buf_dev",
            "(void *)&s->buf",
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("first kernel argument" in item for item in failures))

    def test_upload_order_and_direction_regressions_are_detected(self) -> None:
        sources = _sources()
        sources["host"] = _sub_exact(
            sources["host"],
            r"(hipMemcpy\s*\(\s*dev\s*,\s*&\s*s\s*->\s*buf\s*,\s*"
            r"sizeof\s*\(\s*s\s*->\s*buf\s*\)\s*,\s*)hipMemcpyHostToDevice",
            r"\1hipMemcpyDeviceToHost",
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("adm_hip_upload_buf" in item for item in failures))

    def test_cleanup_regression_is_detected(self) -> None:
        sources = _sources()
        sources["host"] = _sub_exact(
            sources["host"],
            r"adm_hip_free_buf_dev\s*\(\s*s\s*\)\s*;\s*" r"(?=adm_hip_free_luma\s*\(\s*s\s*\))",
            "",
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("dictionary failure" in item for item in failures))

    def test_upload_cleanup_must_remain_in_failure_branch(self) -> None:
        sources = _sources()
        sources["host"] = _sub_exact(
            sources["host"],
            r"if\s*\(\s*err\s*\)\s*\{\s*"
            r"(?P<cleanup>adm_hip_free_luma\s*\(\s*s\s*\)\s*;\s*"
            r"adm_hip_free_buffers\s*\(\s*s\s*\)\s*;\s*"
            r"adm_hip_unload_modules\s*\(\s*s\s*\)\s*;\s*"
            r"adm_hip_destroy_stream\s*\(\s*s\s*\)\s*;)\s*\}\s*"
            r"(?P<result>return\s+err\s*;)",
            r"if (err) {}\n\g<cleanup>\n\g<result>",
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("upload failure" in item for item in failures))

    def test_copy_cleanup_must_remain_in_failure_branch(self) -> None:
        sources = _sources()
        sources["host"] = _sub_exact(
            sources["host"],
            r"if\s*\(\s*hip_err\s*!=\s*hipSuccess\s*\)\s*\{\s*"
            r"\(\s*void\s*\)\s*hipFree\s*\(\s*dev\s*\)\s*;\s*"
            r"(?P<result>return\s+hip_rc\s*\(\s*hip_err\s*\)\s*;)\s*\}",
            r"if (hip_err != hipSuccess) { \g<result> }\n(void)hipFree(dev);",
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("copy failure" in item for item in failures))

    def test_reduce_kernel_stays_buffer_free(self) -> None:
        sources = _sources()
        sources["cm"] = _sub_exact(
            sources["cm"],
            r"(__global__\s+void\s+adm_cm_reduce_line_kernel_4\s*\(\s*)",
            r"\1const AdmBufferHip *buf_ptr, ",
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("buffer-free reduce" in item for item in failures))


if __name__ == "__main__":
    unittest.main()
