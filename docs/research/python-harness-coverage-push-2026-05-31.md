# Python harness coverage push round 2 (2026-05-31)

<!-- Copyright 2026 Lusoris -->

## Summary

Round-2 coverage uplift on pure-Python utility surfaces under
`compat/python-vmaf/`. Round 1 (PR #412 —
`test/compat-python-vmaf-coverage`) targeted `config.py` + 7 small
`tools/` leaves; this round picks up the next-tier modules with the
biggest LOC-weighted gaps remaining: `__init__.py`, `tools/misc.py`,
`tools/sigproc.py`, `tools/scanf.py`, `tools/testutils.py`,
`tools/kimchi.py`, and `core/mixin.py`.

Strict scope discipline applies (CLAUDE.md §8): no Netflix golden
`assertAlmostEqual` assertion was modified; no test in this PR
invokes the `vmaf` binary subprocess. The
`ExternalProgramCaller.call_vmafexec` shell-command builder is
covered by mocking `vmaf.run_process` and asserting on the assembled
command string rather than running the binary.

## Environment

```text
Python  : 3.14.5
pytest  : 9.0.3
pytest-cov : 7.1.0
PYTHONPATH : compat/
Branch  : test/python-test-coverage-push (worktree at /tmp/wt-pythest)
Date    : 2026-05-31
```

The coverage measurement is taken with the existing "safe" test
subset (the same pytest invocation pattern PR #412 used —
`asset_test.py reader_test.py bd_rate_calculator_test.py
calculate_bd_rate_test.py perf_metric_test.py`) extended with the
new file. Tests that require the `vmaf` binary are intentionally
excluded from the measurement set because they would also exercise
modules unrelated to this PR's surface and inflate the headline
numbers.

## Per-module coverage delta

| Module | Baseline | After | Delta |
| --- | ---: | ---: | ---: |
| `compat/python-vmaf/__init__.py` | 24% | 67% | +43 |
| `compat/python-vmaf/core/mixin.py` | 70% | 96% | +26 |
| `compat/python-vmaf/tools/kimchi.py` | 0% | 75% | +75 |
| `compat/python-vmaf/tools/misc.py` | 34% | 59% | +25 |
| `compat/python-vmaf/tools/scanf.py` | 24% | 55% | +31 |
| `compat/python-vmaf/tools/sigproc.py` | 46% | 63% | +17 |
| `compat/python-vmaf/tools/testutils.py` | 0% | 53% | +53 |
| **Aggregate suite total** | **16%** | **20%** | **+4 pp** |

(Aggregate denominator covers every module under
`compat/python-vmaf/` including the large `core/*.py` runner /
trainer modules that are intentionally out of scope for this round —
they require the `vmaf` binary subprocess and are covered by the
slower Netflix golden suite.)

## Methodology

1. Worktree on `origin/master` (tip `e81b13352`).
2. Baseline coverage run with the safe-test subset against
   `compat/python-vmaf/`.
3. Identified the next 7 highest-impact untouched modules (no
   overlap with in-flight PR #412 or PR #413 — verified via
   `gh pr view ... --json files`).
4. Added 82 focused cases in
   `python/test/python_harness_coverage_test.py`.
5. Re-measured coverage against the same denominator.

## Modules covered

### `compat/python-vmaf/__init__.py` (24% → 67%)

- Module-level constants (`__version__`, `VMAF_PYTHON_ROOT`,
  `VMAF_ROOT`) — type + path-shape invariants.
- `project_path`, `required` (happy + sad path), `model_path`.
- `convert_pixel_format_ffmpeg2vmafexec` — full coverage of the
  8/10/12/16-bit dispatch table and the unknown-format `assert`.
- `ExternalProgram` class attribute resolution at import time.
- `ProcessRunner.run` — success path, nonzero-exit failure path,
  forced-C-locale env injection, user-supplied env passthrough.
- `run_process(cmd)` returns 0 on success.
- `ExternalProgramCaller.call_vmafexec` command-line builder —
  no-prediction smoke, float features (`float_psnr / float_ssim
  / float_ms_ssim / float_moment`), ssim deprecation assertion,
  `disable_avx` → `--cpumask -1`, subsample/threads emission,
  model with `vif_enhn_gain_limit / adm_enhn_gain_limit /
  motion_force_zero` knob propagation.

### `compat/python-vmaf/core/mixin.py` (70% → 96%)

- `WorkdirEnabled` — unique UUID subdir, `workdir_root` property.
- `TypeVersionEnabled` — `get_type_version_string`,
  `get_cozy_type_version_string`, `_assert_type_version` sad path
  for both TYPE and VERSION, `find_subclass` unique + zero-match
  assertion.

### `compat/python-vmaf/tools/kimchi.py` (0% → 75%)

- `convert()` round-trips a Python 3 pickle (the function tolerates
  py3-pickled input via `encoding="latin1"` — covers the IO path
  and `dill._reverse_typemap` write).

### `compat/python-vmaf/tools/misc.py` (34% → 59%)

- Path helpers: `get_file_name_without_extension`,
  `get_file_name_with_extension`, `get_file_name_extension`,
  `make_absolute_path` (passthrough + assertion path),
  `make_parent_dirs_if_nonexist`.
- Dict helpers: `get_normalized_string_from_dict`,
  `get_hashable_value_tuple_from_dict`,
  `get_unique_str_from_recursive_dict`, `dedup_value_in_dict`,
  `unroll_dict_of_lists`.
- List helpers: `indices`, `index_and_value_of_min`,
  `get_unique_sorted_list`.
- CLI helpers: `get_cmd_option` (present, missing, terminal
  position), `cmd_option_exists`.
- Misc: `empty_object`, `neg_if_even`, `round_up_to_odd`,
  `map_yuv_type_to_bitdepth` (8/10/12/16/unknown branches),
  `Timer` context manager, `find_linear_function_parameters`
  (happy + degenerate + assertion path), `piecewise_linear_mapping`
  (happy + assertion path).
- The remaining ~40% miss is the binary-shelling
  (`check_program_exist`, `parallel_map`, `match_any_files`),
  `NoPrint` stdout redirect, and `QualityRunnerTestMixin` — all of
  which need the runner stack and are exercised by the slower
  Netflix golden suite.

### `compat/python-vmaf/tools/scanf.py` (24% → 55%)

- `sscanf` exercised against the canonical caller patterns from
  `tools/misc.check_scanf_match` — literal prefix + `%0Nd` width +
  literal suffix (`frame%08d.icpf`), the `-1-2+3-4` canonical
  doctest, capture-zero, and an invalid handler character.
- `handleHex`, `handleOct` low-level helpers.
- `CharacterBufferFromIterable`, `makeCharBuffer` iterable
  dispatch and non-iterable rejection.
- `isFileLike`, `isIterable` predicates.
- `FormatError` / `IncompleteCaptureError` ValueError-subclass
  invariant.
- The remaining 45% miss is the implicit-width `%d / %f / %s`
  branches that are broken by a latent bug — see findings below.

### `compat/python-vmaf/tools/sigproc.py` (46% → 63%)

- `_gauss_window` — length, normalisation-to-unity, symmetry.
- `_hp_image` — zero response for constant input.
- `midrank` — no-ties and ties cases.
- `AUC_CI` — positive SE/CI invariants and SE→CI relationship.
- `significanceBinomial` — equal-proportion p-value ≈ 1 and
  large-gap p-value ≪ 0.01.
- The remaining 37% miss is `significanceHM` (loads the
  `Hanley_McNeil.mat` lookup table from disk),
  `_cov_kendall`, `fastDeLong` (needs synthetic
  `samples.ratings` / `spsizes` fixture — left for a later round),
  and matplotlib-touching helpers.

### `compat/python-vmaf/tools/testutils.py` (0% → 53%)

- `replace_uuid` (match + no-match), `replace_root`,
  `remove_redundant_whitespace`, `remove_option` (middle, start,
  not-present), `remove_elements_containing_substring`.
- `get_tidy_mock_call_args_list` — string-arg and list-arg call
  shapes.
- The remaining 47% miss is `assert_equivalent_commands`'s longer
  branch combinations (covered by the module's own doctest, which
  the test runner doesn't currently collect under this PR's
  invocation).

## Findings worth flagging (latent bugs, deliberately not fixed in this PR)

### Bug 1 — `tools/scanf.py` width handling is inverted

```python
def applyWidth(handler):
    if width is None:  # BUG: inverted check
        return makeWidthLimitedHandler(handler, width, ignoreWhitespace=True)
    return handler
```

`makeWidthLimitedHandler` is invoked only when no width was
specified, passing `width=None` into `CappedBuffer` which then
crashes with `TypeError: '<' not supported between instances of
'int' and 'NoneType'` the first time the buffer tries to bound a
read. Explicit-width invocations (`%5d`) silently fall through to
the unbounded handler — the integer parser then consumes more
characters than the user asked for. The only callers that work in
practice are the ones with literal delimiters that bound the
capture for free (`frame%08d.icpf` works because `.icpf` ends the
digit run). Recommend a separate `fix(scanf):` PR that inverts the
check and adds tests for the previously broken paths.

### Bug 2 — `__init__.py:ProcessRunner.run` does not actually force the C locale

```python
env.setdefault("LC_ALL", "C")
env.setdefault("LANG", "C")
```

The intent (per the inline comment) is to force English subprocess
error messages so AssertionError text remains greppable. But
`setdefault` only sets the var when the key is absent. On a dev
host with `LANG=de_DE.UTF-8` in the environment, the subprocess
inherits the German locale and the comment's invariant is
violated. Fix is a one-line `env["LC_ALL"] = "C"` /
`env["LANG"] = "C"`. Out of scope for a coverage-uplift PR.

## Out-of-scope items

- `tools/plot.py` (matplotlib-heavy, 0% baseline) — would need GUI
  backend dance for headless CI.
- `tools/stats.py` (already covered by PR #412 round 1).
- `core/asset.py` — large module already at 84% from
  `asset_test.py`.
- `core/{executor,quality_runner,feature_extractor,
  cambi_feature_extractor,raw_extractor,...}` — all require the
  `vmaf` binary subprocess and live in the Netflix golden suite.

## References

- CLAUDE.md §8 — Netflix golden-data gate (do not modify
  `assertAlmostEqual` values).
- ADR-0042 — tiny-AI doc + test substance rule (mirror principle
  applied here to the Python harness).
- ADR-0100 — project-wide doc substance rule.
- ADR-0108 — deep-dive deliverables rule.
- PR #412 — round-1 coverage uplift on `compat/python-vmaf/`.
- PR #413 — `fix(decorator): encode str before hashlib.sha1`
  (independent bug-fix for `tools/decorator.py`, no overlap with
  this PR).
