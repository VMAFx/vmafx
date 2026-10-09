#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the option tables, flags and features of every Metal twin to its CPU extractor's.

T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26 (option tables of
integer_psnr_hvs_metal, integer_cambi_metal, ssimulacra2_metal and
integer_vif_metal; ADR-1498): test_metal_twin_option_parity.c compares every
Metal twin's table, TEMPORAL flag and provided features with its CPU
extractor's on an Apple device. This contract makes the same comparison from
the sources (core/test/metal_option_tables.py), so a table drifts here first:

- the option tables are equal in both directions (name, alias, type,
  default, range, flags), except the gaps listed in KNOWN_GAPS, which must
  match exactly: a gap that closes or a new one fails the test;
- a CPU TEMPORAL extractor has a TEMPORAL twin and --subsample treats both
  alike (TEMPORAL or PREV_REF on both or neither);
- the twin provides every feature its CPU extractor provides;
- each option the twin declares is executed: psnr_hvs's `enable_chroma`
  (luma-only combine), ssimulacra2's `yuv_matrix`, vif's three options, and
  cambi's `src_width` / `src_height` / `full_ref`, which this port added (the
  CPU's dimension checks and source window through cambi_internal.h, the
  reference picture through the same pipeline, `cambi_source` and
  `cambi_full_reference` as cambi.c::extract emits them), and cambi's
  `heatmaps_path`, written by cambi.c's own heatmap writers, which
  cambi_internal.h exports (T-METAL-CAMBI-SCORE-NAME-SUFFIXED-2026-10-05);
- every twin names its features from the options as the caller set them:
  init builds the feature-name dictionary before it, or a function it calls
  on the way there, writes an option slot, as cambi.c::init does. A slot
  written first (integer_cambi_metal's resolved encode and source sizes)
  suffixes every emitted name, and a model or test that reads the CPU's name
  finds no score (T-METAL-CAMBI-SCORE-NAME-SUFFIXED-2026-10-05).

Device-free: reads the sources only.
"""

from __future__ import annotations

import functools
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import metal_option_tables as tables

FEATURE = tables.FEATURE
# twin key -> (CPU source, Metal source)
PAIRS = {
    "psnr_hvs": ("third_party/xiph/psnr_hvs.c", "metal/integer_psnr_hvs_metal.mm"),
    "cambi": ("cambi.c", "metal/integer_cambi_metal.mm"),
    "ssimulacra2": ("ssimulacra2.c", "metal/ssimulacra2_metal.mm"),
    "vif": ("integer_vif.c", "metal/integer_vif_metal.mm"),
    # Every other Metal twin (ADR-1498): the pairs of
    # test_metal_twin_option_parity.c, compared from the sources.
    "psnr": ("integer_psnr.c", "metal/integer_psnr_metal.mm"),
    "ssim": ("integer_ssim.c", "metal/integer_ssim_metal.mm"),
    "float_ssim": ("float_ssim.c", "metal/float_ssim_metal.mm"),
    "float_ms_ssim": ("float_ms_ssim.c", "metal/float_ms_ssim_metal.mm"),
    "float_motion": ("float_motion.c", "metal/float_motion_metal.mm"),
    "motion": ("integer_motion.c", "metal/integer_motion_metal.mm"),
    "motion_v2": ("integer_motion_v2.c", "metal/integer_motion_v2_metal.mm"),
    "float_psnr": ("float_psnr.c", "metal/float_psnr_metal.mm"),
    "float_moment": ("float_moment.c", "metal/float_moment_metal.mm"),
    "float_vif": ("float_vif.c", "metal/float_vif_metal.mm"),
    "float_adm": ("float_adm.c", "metal/float_adm_metal.mm"),
    "adm": ("integer_adm.c", "metal/integer_adm_metal.mm"),
    "ciede": ("ciede.c", "metal/integer_ciede_metal.mm"),
}
KNOWN_GAPS = {
    # Every float_adm twin runs the default CSF mode only and marks the
    # option default-only, so another mode keeps the CPU (ADR-1316).
    "float_adm": [
        "adm_csf_mode: cpu Option(name='adm_csf_mode', alias='csf', type='VMAF_OPT_TYPE_INT', "
        "default=0, min=0.0, max=9.0, flags=frozenset({'VMAF_OPT_FLAG_FEATURE_PARAM'})) "
        "twin Option(name='adm_csf_mode', alias='csf', type='VMAF_OPT_TYPE_INT', default=0, "
        "min=0.0, max=9.0, flags=frozenset({'VMAF_OPT_FLAG_DEFAULT_ONLY', "
        "'VMAF_OPT_FLAG_FEATURE_PARAM'}))"
    ],
}

FROZENSET = re.compile(r"frozenset\(\{([^}]*)\}\)")
REGISTRATION = re.compile(r"VmafFeatureExtractor\s+vmaf_fex_\w+\s*=\s*\{")
TEMPORAL = "VMAF_FEATURE_EXTRACTOR_TEMPORAL"
PREV_REF = "VMAF_FEATURE_EXTRACTOR_PREV_REF"

# What executes each declared option on the twin.
EXECUTED = {
    "psnr_hvs": (
        "if (!s->enable_chroma) {",
        "s->n_planes = 1u;",
        # The CPU's luma-only or weighted combine, through the shared host
        # tail (ADR-1397).
        "const double combined = vmaf_psnr_hvs_combined_score(plane_score, s->n_planes);",
    ),
    "ssimulacra2": ("switch (s->yuv_matrix) {",),
    "vif": (
        ".skip_scale0 = s->vif_skip_scale0,",
        ".debug = s->debug,",
        "const VmafMtlGainLimit egl = vmaf_mtl_ivif_make_gain_limit(s->vif_enhn_gain_limit);",
    ),
    "cambi": (
        "if (s->src_width == 0 || s->src_height == 0) {",
        "!cambi_validate_dimensions((unsigned)s->src_width, (unsigned)s->src_height)",
        "(s->src_width > s->enc_width && s->src_height < s->enc_height) ||",
        "(s->src_width < s->enc_width && s->src_height > s->enc_height)",
        "s->src_window = vmaf_cambi_adjust_window(s->window_size, (unsigned)s->src_width,",
        "return vmaf_cambi_check_window_fits_lut(s->adjusted_window, s->src_window);",
        "if (err == 0 && s->full_ref) {",
        "err = cambi_metal_score(s, ref_pic, (unsigned)s->src_width, (unsigned)s->src_height,",
        '"cambi_source", cambi_metal_cap(s, s->src_score),',
        "const double combined = (0 > diff) ? 0 : diff;",
        '"cambi_full_reference",',
        "return (score < s->cambi_max_val) ? score : s->cambi_max_val;",
        "err = vmaf_cambi_open_heatmaps(s->heatmaps_path, (unsigned)s->enc_width,",
        "const int err = vmaf_cambi_dump_c_values(s->heatmaps_files, s->buffers.c_values,",
        "const unsigned *heatmap_frame = (s->heatmaps_path != NULL) ? &index : NULL;",
        "const int heatmaps = vmaf_cambi_close_heatmaps(s->heatmaps_files);",
    ),
}
# A local copy of a CPU cambi helper (the window divisor, the mask index).
CAMBI_COPIES = re.compile(r"\b375u\b|cambi_metal_adjust_window|cambi_metal_get_mask_index")
# What the CPU extractors must keep doing for the twins to mirror them.
REFERENCE_LINES = {
    "cambi.c": (
        "if (s->src_width == 0 || s->src_height == 0) {",
        "if (s->src_width > s->enc_width && s->src_height < s->enc_height) {",
        "return vmaf_cambi_check_window_fits_lut(s->window_size, s->src_window_size);",
        '"cambi_source", MIN(src_score, s->cambi_max_val), index);',
        "double combined_score = combine_dist_src_scores(dist_score, src_score);",
        "return MAX(0, dist_score - src_score);",
        "return open_heatmaps(path, enc_width, enc_height, files);",
        "return dump_c_values(files, c_values, width, height, scale, window_size, num_diffs,",
        "return close_heatmap_files(files);",
    ),
}

# Feature names follow the options as the caller set them (cambi.c::init):
# the feature-name dictionary comes before any write to an option slot, in
# init or in a function init calls before it.
NAME_DICT = "vmaf_feature_name_dict_from_provided_features("
CALLED = re.compile(r"\b(\w+)\s*\(")
DEFINITION = re.compile(r"\b(\w+)\s*\((?:[^;{}()]|\([^()]*\))*\)\s*\{")
KEYWORDS = frozenset({"if", "for", "while", "switch", "return", "sizeof", "catch"})
SLOT_WRITE = re.compile(r"\b\w+\s*->\s*(\w+)\s*=(?!=)")
OPTION_SLOT = re.compile(r"offsetof\(\s*\w+\s*,\s*(\w+)\s*\)")
# How deep the walk follows calls, and how many functions it reads at most.
CALL_DEPTH = 6
MAX_VISITS = 256


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(tables.strip_comments(source).split())


def _sources() -> dict[str, str]:
    names = {name for pair in PAIRS.values() for name in pair} | {DISPATCH}
    names |= {f"metal/{p.name}" for p in (FEATURE / "metal").glob("*.mm")}
    return {name: (FEATURE / name).read_text(encoding="utf-8") for name in sorted(names)}


def _registration(code: str) -> dict[str, str]:
    """`.field = value` items of the file's VmafFeatureExtractor definition."""
    match = REGISTRATION.search(code)
    if not match:
        return {}
    end = tables.balanced_end(code, match.end() - 1)
    fields: dict[str, str] = {}
    for item in tables.split_top(code[match.end() : end]):
        key, _, value = item.partition("=")
        fields[key.strip().lstrip(".")] = value.strip()
    return fields


def _provided(code: str, fields: dict[str, str]) -> set[str]:
    name = fields.get("provided_features", "")
    match = re.search(rf"\b{re.escape(name)}\s*\[\s*\]\s*=\s*\{{", code) if name else None
    if not match:
        return set()
    body = code[match.end() : tables.balanced_end(code, match.end() - 1)]
    return set(re.findall(r'"([^"]+)"', body))


def _flags(fields: dict[str, str]) -> set[str]:
    return set(re.findall(r"VMAF_FEATURE_EXTRACTOR_\w+", fields.get("flags", "")))


def _normal(difference: str) -> str:
    """A difference with each frozenset's items sorted: set order follows the
    interpreter's string hash, which differs between runs."""
    return FROZENSET.sub(
        lambda m: "frozenset({" + ", ".join(sorted(m.group(1).split(", "))) + "})", difference
    )


def _pair_failures(key: str, sources: dict[str, str]) -> list[str]:
    cpu_name, twin_name = PAIRS[key]
    cpu_code = tables.strip_comments(sources[cpu_name])
    twin_code = tables.strip_comments(sources[twin_name])
    found = [
        _normal(item)
        for item in tables.differences(
            tables.table_of_text(sources[cpu_name], FEATURE / cpu_name),
            tables.table_of_text(sources[twin_name], FEATURE / twin_name),
        )
    ]
    failures = []
    if found != KNOWN_GAPS.get(key, []):
        failures.append(
            f"{twin_name}: option table differences {found}, recorded gaps "
            f"{KNOWN_GAPS.get(key, [])}"
        )
    cpu_reg, twin_reg = _registration(cpu_code), _registration(twin_code)
    cpu_flags, twin_flags = _flags(cpu_reg), _flags(twin_reg)
    if TEMPORAL in cpu_flags and TEMPORAL not in twin_flags:
        failures.append(f"{twin_name}: the CPU is TEMPORAL and the twin is not")
    if bool(cpu_flags & {TEMPORAL, PREV_REF}) != bool(twin_flags & {TEMPORAL, PREV_REF}):
        failures.append(f"{twin_name}: --subsample skips frames on one side only")
    missing = _provided(cpu_code, cpu_reg) - _provided(twin_code, twin_reg)
    if missing or not _provided(twin_code, twin_reg):
        failures.append(f"{twin_name}: does not provide {sorted(missing)}")
    return failures


def _executed_failures(sources: dict[str, str]) -> list[str]:
    failures = []
    for key, pieces in EXECUTED.items():
        twin_name = PAIRS[key][1]
        code = _flat(sources[twin_name])
        failures += [
            f"{twin_name}: the option code `{p}` is missing"
            for p in pieces
            if " ".join(p.split()) not in code
        ]
    if CAMBI_COPIES.search(_flat(sources[PAIRS["cambi"][1]])):
        failures.append(f"{PAIRS['cambi'][1]}: a local copy of a CPU cambi helper is back")
    for name, lines in REFERENCE_LINES.items():
        code = _flat(sources[name])
        failures += [f"{name} no longer holds `{line}`" for line in lines if line not in code]
    return failures


@functools.lru_cache(maxsize=64)
def _functions(code: str) -> dict[str, str]:
    """Name -> body of every function `code` defines (first definition wins)."""
    found: dict[str, str] = {}
    for match in DEFINITION.finditer(code):
        name = match.group(1)
        if name in KEYWORDS or name in found:
            continue
        found[name] = code[match.end() : tables.balanced_end(code, match.end() - 1)]
    return found


def _body(code: str, name: str) -> str | None:
    """The body of the function `name` defined in `code`, or None."""
    return _functions(code).get(name)


def _naming(code: str) -> set[str]:
    """The functions of `code` that build the feature-name dictionary, directly
    or through a function of `code` they call (at most CALL_DEPTH calls deep)."""
    bodies = _functions(code)
    naming = {name for name, body in bodies.items() if NAME_DICT in body}
    for _ in range(CALL_DEPTH):
        grown = {name for name, body in bodies.items() if set(CALLED.findall(body)) & naming}
        if grown <= naming:
            break
        naming |= grown
    return naming


def _before_names(code: str, init: str) -> list[str] | None:
    """The code init runs before the dictionary: init's text up to the call
    that leads to it, that function's text up to its next step, and so on;
    None when init never builds the dictionary."""
    naming = _naming(code)
    texts: list[str] = []
    name = init
    for _ in range(CALL_DEPTH):
        body = _body(code, name)
        if body is None:
            return None
        at = body.find(NAME_DICT)
        if at >= 0:
            return [*texts, body[:at]]
        step = next((m for m in CALLED.finditer(body) if m.group(1) in naming), None)
        if step is None:
            return None
        texts.append(body[: step.start()])
        name = step.group(1)
    return None


def _slot_writes(code: str, texts: list[str]) -> set[str]:
    """Option slots written in `texts` or in the functions of `code` they call."""
    slots = set(OPTION_SLOT.findall(code))
    work = [(text, 0) for text in texts]
    seen: set[str] = set()
    found: set[str] = set()
    for _ in range(MAX_VISITS):
        if not work:
            break
        text, depth = work.pop()
        found |= {field for field in SLOT_WRITE.findall(text) if field in slots}
        if depth >= CALL_DEPTH:
            continue
        for callee in sorted(set(CALLED.findall(text)) - seen):
            seen.add(callee)
            body = _body(code, callee)
            if body is not None:
                work.append((body, depth + 1))
    return found


def _name_order_failures(sources: dict[str, str]) -> list[str]:
    failures = []
    for name in sorted(n for n in sources if n.startswith("metal/") and n.endswith(".mm")):
        code = tables.strip_comments(sources[name])
        init = _registration(code).get("init", "")
        texts = _before_names(code, init) if init else None
        if texts is None:
            failures.append(f"{name}: init builds no feature-name dictionary")
            continue
        writes = _slot_writes(code, texts)
        if writes:
            failures.append(
                f"{name}: init writes option slot(s) {sorted(writes)} before it "
                "builds the feature-name dictionary"
            )
    return failures


DISPATCH = "../metal/dispatch_strategy.c"
DISPATCH_TABLE = re.compile(r"g_metal_features\[\]\s*=\s*\{(.*?)\bNULL\b", re.S)


def _dispatch_failures(sources: dict[str, str]) -> list[str]:
    """core/src/metal/dispatch_strategy.c names every Metal extractor and
    every feature it provides, and nothing else: a name missing there sends
    that feature back to the CPU without a word (ADR-0421)."""
    table = DISPATCH_TABLE.search(tables.strip_comments(sources[DISPATCH]))
    listed = set(re.findall(r'"([^"]+)"', table.group(1))) if table else set()
    wanted: set[str] = set()
    for name in sorted(n for n in sources if n.startswith("metal/") and n.endswith(".mm")):
        code = tables.strip_comments(sources[name])
        fields = _registration(code)
        wanted |= set(re.findall(r'"([^"]+)"', fields.get("name", "")))
        wanted |= _provided(code, fields)
    return [f"{DISPATCH} lacks {n}" for n in sorted(wanted - listed)] + [
        f"{DISPATCH} lists {n}, which no Metal extractor provides" for n in sorted(listed - wanted)
    ]


def _contract_failures(sources: dict[str, str]) -> list[str]:
    failures = []
    for key in PAIRS:
        failures += _pair_failures(key, sources)
    return (
        failures
        + _executed_failures(sources)
        + _dispatch_failures(sources)
        + _name_order_failures(sources)
    )


class MetalTwinOptionTablesContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_missing_full_ref_is_detected(self) -> None:
        # The pre-port cambi table: no src_width, src_height or full_ref.
        failures = self._edited(PAIRS["cambi"][1], '.name        = "full_ref",', '.name = "x",')
        self._assert_detected(failures, "the twin lacks full_ref")

    def test_missing_heatmaps_path_is_detected(self) -> None:
        # The table before this port: every CPU option but heatmaps_path.
        failures = self._edited(
            PAIRS["cambi"][1], '.name        = "heatmaps_path",', '.name = "x",'
        )
        self._assert_detected(failures, "the twin lacks heatmaps_path")

    def test_heatmaps_path_without_writer_is_detected(self) -> None:
        # Declaring the option without writing the c-values.
        failures = self._edited(
            PAIRS["cambi"][1],
            "const int err = vmaf_cambi_dump_c_values(",
            "const int err = 0 * vmaf_cambi_test_dump(",
        )
        self._assert_detected(failures, "the option code")

    def test_option_slot_written_before_names_is_detected(self) -> None:
        # A slot written in init itself, before the dictionary.
        failures = self._edited(
            PAIRS["cambi"][1],
            "    IntegerCambiStateMetal *s = (IntegerCambiStateMetal *)fex->priv;\n",
            "    IntegerCambiStateMetal *s = (IntegerCambiStateMetal *)fex->priv;\n"
            "    s->enc_width = (int)w;\n",
        )
        self._assert_detected(failures, "init writes option slot(s) ['enc_width']")

    def test_names_after_resolved_sizes_are_detected(self) -> None:
        # The defect of the M4 Pro report (#2118): the encode and source
        # sizes resolved into their option slots by a helper, then the names.
        failures = self._edited(
            PAIRS["cambi"][1],
            "    s->feature_name_dict =\n        vmaf_feature_name_dict_from_provided_features(",
            "    (void)cambi_metal_resolve_dimensions(s, bpc, w, h);\n"
            "    s->feature_name_dict =\n        vmaf_feature_name_dict_from_provided_features(",
        )
        self._assert_detected(
            failures,
            "integer_cambi_metal.mm: init writes option slot(s) ['enc_bitdepth', "
            "'enc_height', 'enc_width', 'src_height', 'src_width']",
        )

    def test_slot_written_on_the_way_to_names_is_detected(self) -> None:
        # float_motion_metal builds the dictionary in a helper init calls.
        failures = self._edited(
            "metal/float_motion_metal.mm",
            "    fex->extract = extract_force_zero_metal;\n",
            "    s->motion_max_val = 0.0;\n    fex->extract = extract_force_zero_metal;\n",
        )
        self._assert_detected(failures, "float_motion_metal.mm: init writes option slot(s)")

    def test_missing_names_are_detected(self) -> None:
        failures = self._edited(
            "metal/integer_psnr_metal.mm",
            "vmaf_feature_name_dict_from_provided_features(",
            "vmaf_feature_names_elsewhere(",
        )
        self._assert_detected(failures, "integer_psnr_metal.mm: init builds no feature-name")

    def test_default_drift_is_detected(self) -> None:
        failures = self._edited(
            PAIRS["psnr_hvs"][1], ".default_val = {.b = true},", ".default_val = {.b = false},"
        )
        self._assert_detected(failures, "enable_chroma: cpu")

    def test_missing_provided_feature_is_detected(self) -> None:
        failures = self._edited(
            PAIRS["psnr_hvs"][1], '"psnr_hvs_cr", "psnr_hvs",', '"psnr_hvs_cr",'
        )
        self._assert_detected(failures, "does not provide ['psnr_hvs']")

    def test_subsample_flag_mismatch_is_detected(self) -> None:
        failures = self._edited(
            PAIRS["vif"][1],
            ".flags             = VMAF_FEATURE_EXTRACTOR_METAL,",
            ".flags             = VMAF_FEATURE_EXTRACTOR_METAL | VMAF_FEATURE_EXTRACTOR_TEMPORAL,",
        )
        self._assert_detected(failures, "--subsample skips frames on one side only")

    def test_full_ref_not_run_on_the_reference_is_detected(self) -> None:
        failures = self._edited(
            PAIRS["cambi"][1], "    if (err == 0 && s->full_ref) {", "    if (err == 0 && false) {"
        )
        self._assert_detected(failures, "the option code")

    def test_local_window_copy_is_detected(self) -> None:
        failures = self._edited(
            PAIRS["cambi"][1],
            "static int cambi_metal_resolve_windows(",
            "static unsigned cambi_metal_adjust_window(unsigned v) { return v / 375u; }\n"
            "static int cambi_metal_resolve_windows(",
        )
        self._assert_detected(failures, "local copy of a CPU cambi helper")

    def test_missing_float_adm_override_is_detected(self) -> None:
        failures = self._edited(
            "metal/float_adm_metal.mm", '{.name = "adm_f2s3",', '{.name = "adm_f2s3_gone",'
        )
        self._assert_detected(failures, "float_adm_metal.mm: option table differences")

    def test_dispatch_table_drift_is_detected(self) -> None:
        failures = self._edited(DISPATCH, '    "VMAF_integer_feature_motion3_score",\n', "")
        self._assert_detected(failures, "lacks VMAF_integer_feature_motion3_score")
        failures = self._edited(
            DISPATCH,
            '    "integer_motion_metal",\n',
            '    "integer_motion_metal",\n    "motion_y",\n',
        )
        self._assert_detected(failures, "lists motion_y")

    def test_cpu_reference_drift_is_detected(self) -> None:
        failures = self._edited(
            "cambi.c", "return MAX(0, dist_score - src_score);", "return dist_score - src_score;"
        )
        self._assert_detected(failures, "cambi.c no longer holds")


if __name__ == "__main__":
    unittest.main()
