<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1007: Fix C string/numeric UB cluster — NULL strcmp, size_t underflow, signed-shift overflow, snprintf truncation

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `core`, `security`, `c`, `ub`

## Context

Static analysis and audit (r3/r4 review round) identified four related undefined-behaviour
and correctness defects in the C core, all in paths that are reachable from normal user input:

1. **`feature_name.c:108`** — `strcmp(opt->default_val.s, *((char**)data))` is called
   unconditionally. Any `VMAF_OPT_TYPE_STRING` option registered with a NULL default
   (`default_val.s = NULL`) or invoked before the option value is set produces
   `strcmp(NULL, ...)`, which is UB and crashes on most implementations.
   The same NULL pointer is later passed to `snprintf(..., "%s", ...)` at line 152.

2. **`model.c:247`** — `strlen(model->name) - 5` is `size_t` arithmetic; when the model
   name is shorter than five characters the subtraction wraps to `SIZE_MAX - 4`, feeding
   a near-`SIZE_MAX` value into `calloc`. On most platforms this OOM-fails silently; on a
   64-bit system with overcommit it produces a heap overwrite via the subsequent `strncpy`.

3. **`integer_adm.c:1114,1119,1124`** — `1 << (14 + kh_shift)` where `kh_shift` can reach 17
   makes the shift operand 31 on a signed `int` literal. C signed left-shift into the sign
   bit is UB; compilers with `-fstrict-overflow` are permitted to elide the entire
   expression or produce wrong values. Mirrored at line 2099 for the width-direction.

4. **`integer_adm.c:1563-1566`** — `uint32_t add_shift_cub = (uint32_t)pow(2, (shift_cub - 1))`
   where `shift_cub` is computed from `ceil(log2(right-left))`. When `right-left == 1`,
   `log2(1) = 0`, `shift_cub = 0`, and `shift_cub - 1` wraps to `UINT32_MAX`.
   `pow(2, UINT32_MAX)` is +Inf; casting +Inf to `uint32_t` is UB. Second instance at
   line 2104.

5. **`cambi.c:735`** — `(void)snprintf(path, sizeof(path), ...)` discards the return value.
   A long `heatmaps_path` option value silently truncates the constructed file path; the
   subsequent `open()` creates a file at the wrong location with no diagnostic.

None of these are reachable via the standard Netflix model paths, but they are reachable
via the public API (custom models, custom options, unusual frame dimensions).

## Decision

Fix all five defects in-place with the minimal-diff approach:

- `feature_name.c` `option_is_default`: guard both string operands before `strcmp`;
  `vmaf_feature_name_from_options`: null-check the string pointer before `snprintf`.
- `model.c`: add `if (strlen(model->name) < 5) goto fail_mc;` before the subtraction.
- `integer_adm.c` shift literals: change `1` to `1u` and cast result to `int64_t` to
  keep the arithmetic unsigned before widening.
- `integer_adm.c` `pow(2, shift-1)`: guard with `(shift > 0u) ? ... : 0u`.
- `cambi.c`: capture `snprintf` return value and return `-ENAMETOOLONG` on truncation.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Suppress with NOLINT | Zero diff | Masks real crash/UB | No: ADR-0278 requires inline justification for load-bearing invariants only |
| Add assertions in tests only | Catches regression | Does not fix the UB | Insufficient: UB may be exploited before the test fires |
| Minimal in-place guard | Tiny diff, no logic change | None | Chosen |

## Consequences

- **Positive**: Eliminates five UB sources reachable via public API; `UBSan` and
  `ASan` will no longer fire on these paths.
- **Neutral**: No change to any Netflix golden-data assertion values. The guarded
  paths are only active for non-default / unusual option values.
- **Negative**: None.

## References

- r3/r4 audit findings: `[r3-string-handling]` and `[r3-numeric-ub]` clusters
- ADR-0141 (touched-file cleanup rule)
- ADR-0278 (NOLINT citation closeout)
