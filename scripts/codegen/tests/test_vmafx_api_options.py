#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Option-group emitters (RC4 WP8, ADR-1852): CLI table and usage, MCP schemas
and argument-vector spec, proto messages, OpenAPI components, the FFmpeg
AVOption table and the documentation tables. Each renders the every-feature
fixture, each validation rule refuses its planted defect, and the outputs a
tool can read are read by that tool (protoc, buf, a C compiler with libavutil)
when it is installed (skipped with the reason otherwise).
"""

from __future__ import annotations

import copy
import json
import shutil
import tempfile
import unittest
from pathlib import Path
from typing import Any

from support import ROOT, document, entry, fixture, run, tool
from vmafx_api import (
    emit_cli,
    emit_ffmpeg_options,
    emit_mcp,
    emit_openapi,
    emit_option_docs,
    emit_proto,
)
from vmafx_api.loader import parse
from vmafx_api.model import Api, DefinitionError
from vmafx_api.splice import splice


def api() -> Api:
    return parse(fixture())


def refused(change: dict[str, Any], group: str, index: int, text: str) -> None:
    doc = copy.deepcopy(fixture())
    option = entry(doc["option_groups"], group)["options"][index]
    for key, value in change.items():
        if value is None:
            option.pop(key, None)
        else:
            option[key] = value
    with unittest.TestCase().assertRaisesRegex(DefinitionError, text):
        parse(doc)


def build_and_run(test: unittest.TestCase, header: str, program: str) -> None:
    """Compile `program` with the generated `header` against libavutil and run it;
    skipped without a C compiler or libavutil."""
    compiler, pkg = tool("cc"), tool("pkg-config")
    probe = run([pkg, "--cflags", "--libs", "libavutil"]) if pkg else None
    flags = probe.stdout.split() if probe is not None and probe.returncode == 0 else []
    if compiler is None or not flags:
        test.skipTest("no C compiler or no libavutil (pkg-config libavutil)")
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "vf_vmafx_options.h").write_text(header)
        (Path(tmp) / "t.c").write_text(program)
        exe = str(Path(tmp) / "t")
        build = run(
            [
                str(compiler),
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{tmp}",
                f"{tmp}/t.c",
                "-o",
                exe,
                *flags,
            ]
        )
        test.assertEqual(build.returncode, 0, build.stderr)
        result = run([exe])
    test.assertEqual(result.returncode, 0, f"option check {result.returncode} failed")


class CliEmitterTest(unittest.TestCase):
    def test_short_string_long_table_and_identifiers(self) -> None:
        a = api()
        self.assertEqual(emit_cli.short_string(a), "r:b:m:")
        entries = {name: (has_arg, ident) for name, has_arg, ident in emit_cli.long_entries(a)}
        self.assertEqual(entries["reference"], (1, "'r'"))
        self.assertEqual(entries["precise-scores"], (0, "ARG_PRECISE"))
        self.assertEqual(entries["json"], (0, "ARG_FORMAT_JSON"))
        self.assertIn("ARG_THREADS", emit_cli.identifiers(a))
        self.assertNotIn("'r'", emit_cli.identifiers(a))

    def test_usage_names_every_spelling_and_default(self) -> None:
        lines = emit_cli.usage_lines(api())
        text = "\n".join(lines)
        self.assertIn(" --reference/-r $path:", text)
        self.assertIn("--precise, --precise-scores:", text)
        self.assertIn("(default: VMAF_DEFAULT_MODEL_VERSION)", text)
        self.assertIn(" --xml:", text)
        self.assertTrue(all(len(line) <= emit_cli.USAGE_WIDTH for line in lines))

    def test_include_compiles_as_cpp(self) -> None:
        compiler = tool("c++")
        if compiler is None:
            self.skipTest("no C++ compiler on PATH")
        source = "#include <cstdint>\n#include <getopt.h>\n" + emit_cli.include_text(api())
        source += "int main() { return long_opts[0].name ? 0 : 1; }\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "t.cpp"
            path.write_text(source)
            result = run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-o",
                    str(Path(tmp) / "t"),
                    str(path),
                ]
            )
        self.assertEqual(result.returncode, 0, result.stderr)


class RulesTest(unittest.TestCase):
    """Every rule the option loader enforces refuses its planted defect."""

    def test_option_rules(self) -> None:
        cases = (
            ({"cli": None}, "input", 4, "no `cli` spelling"),
            ({"cli_values": {"xml": "--xml"}}, "input", 4, "cli_values"),
            ({"default": "x"}, "input", 2, "`default_macro` names a C macro"),
            ({"repeat": True}, "input", 4, "only string options repeat"),
            ({"surface_defaults": {"ffmpeg": 1}}, "input", 1, "surface default for ffmpeg"),
            ({"surfaces": ["cli", "ffmpeg"]}, "input", 1, "subset"),
            ({"cli_short": "b"}, "input", 0, "also used by"),
            ({"choices": [8, 10]}, "input", 6, "`choices` limits an integer option"),
            ({"default": 9}, "input", 1, "not one of"),
            ({"argv": "sometimes"}, "input", 1, "`argv` is one of"),
            ({"name": "Bad-Name"}, "input", 4, "lower_snake_case"),
            ({"mcp_required": True}, "window", 0, "needs the mcp surface"),
            ({"default": ["log", "pipe"]}, "window", 1, "list of its values"),
            ({"range": [5, 1]}, "pool", 1, "minimum is above its maximum"),
        )
        for change, group, index, text in cases:
            with self.subTest(change=change):
                refused(change, group, index, text)

    def test_group_rules(self) -> None:
        doc = copy.deepcopy(fixture())
        del entry(doc["option_groups"], "input")["mcp_tools"]
        with self.assertRaisesRegex(DefinitionError, "mcp_tools"):
            parse(doc)
        doc = copy.deepcopy(fixture())
        entry(doc["option_groups"], "window")["options"][0]["name"] = "threads"
        with self.assertRaisesRegex(DefinitionError, "unique across groups"):
            parse(doc)
        doc = copy.deepcopy(fixture())
        entry(doc["option_groups"], "input")["options"][1]["proto"] = {"field": 2}
        with self.assertRaisesRegex(DefinitionError, "proto:ScoreOptions"):
            parse(doc)


class McpEmitterTest(unittest.TestCase):
    def test_tool_schemas(self) -> None:
        tools = emit_mcp.tool_schemas(api())
        score = tools["score"]
        self.assertEqual(score["$schema"], emit_mcp.DIALECT)
        self.assertEqual(score["required"], ["ref"])
        self.assertEqual(score["properties"]["output_fmt"]["default"], "json")  # surface default
        self.assertEqual(score["properties"]["feature"]["type"], "array")
        self.assertNotIn("default", score["properties"]["model"])  # the library's, not a literal
        self.assertIn("VMAF_DEFAULT_MODEL_VERSION", score["properties"]["model"]["description"])
        self.assertIn("Reserved", score["properties"]["target_width"]["description"])
        self.assertNotIn("threads", tools["inspect"]["properties"])

    def test_argv_spec_order_and_forms(self) -> None:
        argv = emit_mcp.document(api())["argv"]
        forms = {e["option"]: (e["stage"], e["flag"], e["form"]) for e in argv}
        self.assertEqual(forms["reference"], ("core", "-r", "value"))
        self.assertEqual(forms["model"], ("core", "-m", "value"))  # repeats on the CLI only
        self.assertEqual(forms["feature"], ("extra", "--feature", "repeat"))
        self.assertEqual(forms["precise"], ("extra", "--precise", "switch"))
        self.assertEqual(forms["format"][2], "choice")
        self.assertEqual(forms["clip"], ("extra", "", "suffix"))
        self.assertNotIn("target_width", forms)  # no CLI flag, no suffix
        self.assertEqual([e["option"] for e in argv][:2], ["threads", "reference"])

    def test_library_defaults_come_from_the_header(self) -> None:
        defaults = emit_mcp.document(api())["library_defaults"]
        header = (ROOT / "core/include/libvmaf/model.h").read_text()
        self.assertIn(
            f'#define VMAF_DEFAULT_MODEL_VERSION "{defaults["VMAF_DEFAULT_MODEL_VERSION"]}"', header
        )
        doc = copy.deepcopy(fixture())
        entry(doc["option_groups"], "input")["options"][2]["default_macro"] = "NOT_A_DEFINED_MACRO"
        with self.assertRaisesRegex(DefinitionError, "NOT_A_DEFINED_MACRO"):
            emit_mcp.document(parse(doc))

    def test_both_server_copies_are_one_text(self) -> None:
        text = emit_mcp.options_json(api())
        self.assertEqual(json.loads(text)["$comment"], emit_mcp.COMMENT)
        self.assertEqual(len(emit_mcp.OUTPUTS), 2)


class ProtoEmitterTest(unittest.TestCase):
    def test_messages(self) -> None:
        text = emit_proto.proto_text(api())
        self.assertIn("message ScoreOptions {", text)
        self.assertIn("  optional uint32 bitdepth = 1;", text)
        self.assertIn("  repeated string feature = 4;", text)
        self.assertIn("message WindowOptions {", text)
        self.assertIn("  repeated string stats_out = 2;", text)
        self.assertIn("message WindowResult {", text)
        self.assertIn("  repeated double value = 2;", text)
        self.assertNotIn("struct_size", text)

    def test_struct_without_a_proto_form_is_refused(self) -> None:
        doc = copy.deepcopy(fixture())
        entry(doc["structs"], "VmafxFence")["proto"] = "Fence"  # holds a uptr handle
        with self.assertRaisesRegex(DefinitionError, "has no proto form"):
            emit_proto.proto_text(parse(doc))

    def test_protoc_compiles_the_messages(self) -> None:
        protoc = tool("protoc")
        if protoc is None:
            self.skipTest("protoc not on PATH")
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "vmafx_api.proto").write_text(emit_proto.proto_text(api()))
            result = run([protoc, f"-I{tmp}", "--descriptor_set_out=/dev/null", "vmafx_api.proto"])
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_buf_breaking_refuses_a_renumbered_field(self) -> None:
        buf = tool("buf")
        if buf is None:
            self.skipTest("buf not on PATH")
        with tempfile.TemporaryDirectory() as tmp:
            old, new = Path(tmp) / "old", Path(tmp) / "new"
            for target in (old, new):
                shutil.copytree(ROOT / "proto", target)
            planted = (
                (new / "vmafx.proto")
                .read_text()
                .replace("ScoreOptions options = 4;", "ScoreOptions options = 9;")
            )
            (new / "vmafx.proto").write_text(planted)
            clean = run([buf, "breaking", str(old), "--against", str(old)])
            broken = run([buf, "breaking", str(new), "--against", str(old)])
        self.assertEqual(clean.returncode, 0, clean.stdout + clean.stderr)
        self.assertNotEqual(broken.returncode, 0, "buf breaking accepted a renumbered field")


class OpenApiEmitterTest(unittest.TestCase):
    def test_components(self) -> None:
        schemas = emit_openapi.schemas(api())
        self.assertEqual(schemas["ScoreOptions"]["properties"]["bitdepth"]["enum"], [8, 10])
        self.assertEqual(schemas["ScoreOptions"]["properties"]["feature"]["type"], "array")
        self.assertEqual(
            schemas["WindowResult"]["properties"]["value"]["items"]["format"], "double"
        )
        text = emit_openapi.components_text(api())
        self.assertIn('components:\n  schemas:\n    ScoreOptions:\n      type: "object"', text)
        self.assertIn("          enum:\n            - 8\n            - 10", text)


class FfmpegEmitterTest(unittest.TestCase):
    PROGRAM = r"""
#include <stddef.h>
#include <string.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include "vf_vmafx_options.h"
typedef struct Ctx { const AVClass *klass; VMAFX_FILTER_OPTION_FIELDS } Ctx;
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption options[] = { VMAFX_FILTER_OPTIONS(Ctx, FLAGS) {NULL} };
static const AVClass klass = {.class_name = "t", .item_name = av_default_item_name,
                              .option = options, .version = LIBAVUTIL_VERSION_INT};
static const char *const pools[] = VMAFX_OPT_POOL_VALUES;
int main(void)
{
    Ctx c = {.klass = &klass};
    av_opt_set_defaults(&c);
    if (c.threads != 0 || c.subsample != 1 || c.pool != 0 || c.stats_out != 1) return 1;
    if (av_opt_set(&c, "n_threads", "4", 0) < 0 || c.threads != 4) return 2;
    if (av_opt_set(&c, "pool", "harmonic_mean", 0) < 0 || strcmp(pools[c.pool], "harmonic_mean")) return 3;
    if (av_opt_set(&c, "stats_out", "log+file", 0) < 0 || c.stats_out != 3) return 4;
    if (av_opt_set(&c, "threads", "300", 0) >= 0) return 5;
    return 0;
}
"""

    def test_table_builds_and_sets_options_with_libavutil(self) -> None:
        build_and_run(self, emit_ffmpeg_options.header_text(api()), self.PROGRAM)


class LiveFfmpegTableTest(unittest.TestCase):
    """The vmafx filter table of the live definition sets what design 5.2 says."""

    PROGRAM = r"""
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include "vf_vmafx_options.h"
typedef struct Ctx { const AVClass *klass; VMAFX_FILTER_OPTION_FIELDS } Ctx;
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption options[] = { VMAFX_FILTER_OPTIONS(Ctx, FLAGS) {NULL} };
static const AVClass klass = {.class_name = "vmafx", .item_name = av_default_item_name,
                              .option = options, .version = LIBAVUTIL_VERSION_INT};
static const char *const backends[] = VMAFX_OPT_BACKEND_VALUES;
static const char *const formats[] = VMAFX_OPT_OUTPUT_FORMAT_VALUES;
static int errors;
static void count_errors(void *avcl, int level, const char *fmt, va_list vl)
{
    (void)avcl; (void)fmt; (void)vl;
    errors += level <= AV_LOG_ERROR;
}
int main(void)
{
    Ctx c = {.klass = &klass};
    av_log_set_callback(count_errors);
    av_opt_set_defaults(&c);
    /* Every default lies in its option's range: av_opt_set_defaults() logs
     * an error for one that does not, on every filter init (RC4 WP9). */
    if (errors != 0 || c.view_distance != 0.0 || c.display_height != 0) return 6;
    if (c.model != NULL || strcmp(backends[c.backend], "auto") || strcmp(formats[c.output_format], "json"))
        return 1;
    if (c.provenance != 3 || c.subsample != 1 || strcmp(c.device, "auto") || strcmp(c.score_fmt, "%.6f"))
        return 2;
    if (av_opt_set(&c, "n_subsample", "2", 0) < 0 || c.subsample != 2) return 3;
    if (av_opt_set(&c, "backend", "sycl", 0) < 0 || strcmp(backends[c.backend], "sycl")) return 4;
    if (av_opt_set(&c, "backend", "vulkan", 0) >= 0) return 5;
    av_opt_free(&c);
    return 0;
}
"""

    def test_live_table_builds_and_defaults_follow_the_design(self) -> None:
        build_and_run(self, emit_ffmpeg_options.header_text(parse(document())), self.PROGRAM)


class SpliceTest(unittest.TestCase):
    def test_region_is_replaced_and_a_lost_marker_stops_generation(self) -> None:
        begin, end = emit_option_docs.markers("cli options")
        text = f"# Page\n\n{begin}\nold\n{end}\n\nprose\n"
        self.assertEqual(
            splice(text, begin, end, ["new"], "p.md"), f"# Page\n\n{begin}\nnew\n{end}\n\nprose\n"
        )
        with self.assertRaisesRegex(DefinitionError, "marker line"):
            splice(text.replace(end, ""), begin, end, ["new"], "p.md")

    def test_tables_escape_markdown(self) -> None:
        rows = emit_option_docs.table(
            api(), ("Option", "Short", "Value", "Default", "Description"), emit_option_docs._cli_row
        )
        self.assertIn("| `--reference` | `-r` | string | | Reference file. |", rows)
        self.assertTrue(all("|  |" not in row for row in rows))


if __name__ == "__main__":
    unittest.main()
