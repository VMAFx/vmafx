#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Keep HIP integer-ADM's large kernel argument device-resident (ADR-0759)."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

FEATURE_DIR = Path(__file__).resolve().parents[1] / "src" / "feature" / "hip"
SOURCE_PATHS = {
    "host": "integer_adm_hip.c",
    "csf": "integer_adm/adm_csf.hip",
    "cm": "integer_adm/adm_cm.hip",
}
POINTER_DECL = "const AdmBufferHip *__restrict__ buf_ptr"
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
            output.extend("\n" for char in source[index : end + 2] if char == "\n")
            index = end + 2
            continue
        output.append(source[index])
        index += 1
    return "".join(output)


def _normalize(source: str) -> str:
    return re.sub(r"\s+", " ", _strip_comments(source).replace("\\\n", " ")).strip()


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


def _sources(feature_dir: Path = FEATURE_DIR) -> dict[str, str]:
    return {
        role: (feature_dir / relative).read_text(encoding="utf-8")
        for role, relative in SOURCE_PATHS.items()
    }


def _require_order(failures: list[str], scope: str, source: str, markers: tuple[str, ...]) -> None:
    cursor = 0
    for marker in markers:
        found = source.find(marker, cursor)
        if found < 0:
            failures.append(f"{scope}: missing or reordered {marker}")
            return
        cursor = found + len(marker)


def _validate_kernel_signatures(sources: dict[str, str], failures: list[str]) -> None:
    clean_kernels = _strip_comments(sources["csf"] + "\n" + sources["cm"])
    if re.search(r"\bAdmBufferHip\s+(?:buf|buf_ptr)\b", clean_kernels):
        failures.append("kernels: AdmBufferHip must never be passed by value")

    for macro_name in ("ADM_CSF_KERNEL", "I4_ADM_CSF_KERNEL"):
        macro = _macro(sources["csf"], macro_name)
        if f"( {POINTER_DECL}," not in macro:
            failures.append(f"{macro_name}: first argument is not the device buffer pointer")

    for kernel in ("i4_adm_cm_line_kernel", "adm_cm_line_kernel_8"):
        signature = _normalize(_function(sources["cm"], kernel).split("{", 1)[0])
        if f"{kernel}({POINTER_DECL}," not in signature:
            failures.append(f"{kernel}: first argument is not the device buffer pointer")

    reduce_signature = _normalize(
        _function(sources["cm"], "adm_cm_reduce_line_kernel_4").split("{", 1)[0]
    )
    if "AdmBufferHip" in reduce_signature:
        failures.append("adm_cm_reduce_line_kernel_4: buffer-free reduce gained AdmBufferHip")


def _validate_launches(host: str, failures: list[str]) -> None:
    for launcher, kernel_handle in LAUNCHERS.items():
        body = _normalize(_function(host, launcher))
        argument = "void *args[] = {(void *)&s->buf_dev,"
        args_index = body.find(argument)
        launch_index = body.find(kernel_handle)
        if args_index < 0 or launch_index < 0 or args_index > launch_index:
            failures.append(f"{launcher}: first kernel argument is not &s->buf_dev")
        if body.count("&s->buf_dev") != 1:
            failures.append(f"{launcher}: expected exactly one device-buffer argument")


def _validate_upload(host: str, failures: list[str]) -> None:
    upload = _normalize(_function(host, "adm_hip_upload_buf"))
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
    if "hipMalloc((void **)&s->buf_dev" in upload:
        failures.append("adm_hip_upload_buf: publishes the allocation before the copy succeeds")

    free_buf = _normalize(_function(host, "adm_hip_free_buf_dev"))
    _require_order(
        failures,
        "adm_hip_free_buf_dev",
        free_buf,
        ("s->buf_dev != NULL", "hipFree(s->buf_dev)", "s->buf_dev = NULL;"),
    )


def _validate_lifecycle(host: str, failures: list[str]) -> None:
    init_device = _normalize(_function(host, "adm_hip_init_device"))
    _require_order(
        failures,
        "adm_hip_init_device upload",
        init_device,
        ("adm_hip_slice_bands(s, h);", "adm_hip_slice_results(s);", "adm_hip_upload_buf(s);"),
    )
    _require_order(
        failures,
        "adm_hip_init_device upload failure",
        init_device[init_device.find("adm_hip_upload_buf(s);") :],
        (
            "adm_hip_free_luma(s);",
            "adm_hip_free_buffers(s);",
            "adm_hip_unload_modules(s);",
            "adm_hip_destroy_stream(s);",
        ),
    )

    init_fex = _normalize(_function(host, "init_fex_hip"))
    dictionary_failure = init_fex[init_fex.find("s->feature_name_dict == NULL") :]
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

    close = _normalize(_function(host, "close_fex_hip"))
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
    if _normalize(host).count("AdmBufferHip *buf_dev;") != 1:
        failures.append("AdmStateHip: expected one device-resident AdmBufferHip owner")
    validators = (
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
        sources["csf"] = sources["csf"].replace(POINTER_DECL, "AdmBufferHip buf", 1)
        self.assertTrue(any("passed by value" in item for item in _contract_failures(sources)))

    def test_host_launch_regression_is_detected(self) -> None:
        sources = _sources()
        sources["host"] = sources["host"].replace("(void *)&s->buf_dev", "(void *)&s->buf", 1)
        failures = _contract_failures(sources)
        self.assertTrue(any("first kernel argument" in item for item in failures))

    def test_upload_order_and_direction_regressions_are_detected(self) -> None:
        sources = _sources()
        sources["host"] = sources["host"].replace(
            "hipMemcpy(dev, &s->buf, sizeof(s->buf), hipMemcpyHostToDevice)",
            "hipMemcpy(dev, &s->buf, sizeof(s->buf), hipMemcpyDeviceToHost)",
            1,
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("adm_hip_upload_buf" in item for item in failures))

    def test_cleanup_regression_is_detected(self) -> None:
        sources = _sources()
        sources["host"] = sources["host"].replace(
            "        adm_hip_free_buf_dev(s);\n        adm_hip_free_luma(s);",
            "        adm_hip_free_luma(s);",
            1,
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("dictionary failure" in item for item in failures))

    def test_reduce_kernel_stays_buffer_free(self) -> None:
        sources = _sources()
        sources["cm"] = sources["cm"].replace(
            "__global__ void adm_cm_reduce_line_kernel_4(",
            "__global__ void adm_cm_reduce_line_kernel_4(const AdmBufferHip *buf_ptr, ",
            1,
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("buffer-free reduce" in item for item in failures))


if __name__ == "__main__":
    unittest.main()
