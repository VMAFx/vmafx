# Research: SYCL `-fsycl` link-propagation and feature-name aliasing

**Related ADR**: ADR-1099
**Date**: 2026-06-07
**Scope**: SYCL build system, test infrastructure

## Problem statement

`test_sycl_motion_add_uv_parity` consistently crashed with SIGSEGV on Intel Arc
A380 hardware after two previous fix attempts (PRs #768, #796). The test was
marked `should_fail: true` (ADR-1093) as a temporary measure.

## Investigation

### Root cause 1: missing `-fsycl` at link time

**Hypothesis**: The test executable links `libvmaf.a` without `-fsycl`, causing
SYCL device-image registration to be skipped.

**Verification method**: Examined the Ninja build commands for both
`libvmaf.so` and test executables. Confirmed that `libvmaf.so` used
`icpx ... -fsycl ...` while test executables used
`icpx ... -Wl,--start-group src/libvmaf.a libsycl.so ...` without `-fsycl`.

**Mechanism**: When `icpx` compiles SYCL source files with `-fsycl`, it embeds
SPIR-V device images in the resulting `.o` files. At link time, `-fsycl` causes
`icpx` to invoke `clang-offload-wrapper`, which creates a synthetic translation
unit that registers all device images with the SYCL runtime's `ProgramManager`
during library/executable initialization. Without `-fsycl` at link time, this
registration step is silently skipped.

At runtime, `sycl::queue::submit()` calls
`ProgramManager::getDeviceKernelInfo(CompileTimeKernelInfoTy const&)`. This
function looks up a program handle by kernel name; when registration was skipped,
the handle is null, and the null dereference produces a SIGSEGV.

**Meson code path**: `sycl_dependency` is declared in `core/src/meson.build`
without `link_args`. The library gets `-fsycl` via the separate `vmaf_link_args`
variable, but that variable is applied only to the `library()` target. Test
executables that declare `dependencies: [sycl_dependency]` receive no `-fsycl`.

**Fix**: Add `link_args: ['-fsycl']` to the `sycl_dependency` `declare_dependency`
block. Remove `-fsycl` from `vmaf_link_args` to avoid double-passing.

### Root cause 2: feature-name aliasing

**Hypothesis**: `vmaf_feature_score_at_index` returns -EINVAL because the
feature is stored under an aliased name, not the raw name passed to the function.

**Verification method**: Traced `vmaf_feature_name_from_opts_dict` in
`core/src/feature/feature_name.c`. Confirmed:

1. When `opts_dict` is non-empty (any FEATURE_PARAM option is non-default), the
   function replaces the raw name with its canonical alias from `alias.c`.
2. For each active bool option, it appends `_<option_alias>` to the name.

For `VMAF_integer_feature_motion2_score` with `motion_add_uv=true`:

- `vmaf_feature_name_alias("VMAF_integer_feature_motion2_score")` → `"integer_motion2"`
- `motion_add_uv` has alias `"mau"`, type `BOOL` → appends `"_mau"`
- Stored name: `"integer_motion2_mau"`

For `VMAF_feature_motion2_score` with `motion_add_uv=true` (float_motion):

- `vmaf_feature_name_alias("VMAF_feature_motion2_score")` → `"float_motion2"`
- Same suffix → stored name: `"float_motion2_mau"`

`vmaf_feature_collector_get_score` uses exact string matching; no alias lookup.

**Fix**: Update the test to query `"integer_motion2_mau"` (SYCL) and
`"float_motion2_mau"` (CPU). The Y-only baseline with default options (no
non-default FEATURE_PARAM options) stores the raw name
`"VMAF_integer_feature_motion2_score"` — query that unchanged.

## Key invariants established

1. Every Meson target linking SYCL `.o` files from a static archive MUST have
   `-fsycl` at its link step. The `sycl_dependency.link_args` centralizes this.

2. When querying feature scores for an extractor with non-default
   `VMAF_OPT_FLAG_FEATURE_PARAM` options, the stored name is:
   `<alias_of_raw_name>_<opt1_alias>_<opt2_alias>...` (alphabetical option order).
   When all FEATURE_PARAM options are at default, the raw name is stored unchanged.
