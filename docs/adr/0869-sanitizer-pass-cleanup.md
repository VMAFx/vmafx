<!-- markdownlint-disable MD013 MD060 -->
# ADR-0869: Sanitizer-Pass Cleanup — CAMBI Option-Type Mismatch and AVX{2,512} ADM Signed-Shift UB

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris, Claude (sanitizer audit)
- **Tags**: `c`, `simd`, `sanitizer`, `correctness`, `cambi`, `adm`

## Context

Master CI runs a scheduled "Sanitizers + Fuzz" job, but the build-matrix
sanitizer combinations are not run on every PR. A local
`-Db_sanitize=address,undefined` build of `core/` against the full unit-test
suite plus extractor binaries (vmaf CLI, vmaf-perShot, vmaf_roi) surfaced
two real undefined-behaviour findings that the unit tests alone miss
because the relevant code paths are option- and pixel-format-conditional:

1. **CAMBI option-parser writes through misaligned `uint16_t` slots.**
   The options table declares `window_size` and `max_log_contrast` as
   `VMAF_OPT_TYPE_INT`, but the underlying `CambiState` fields are
   `uint16_t`. The parser's `set_option_int` performs `*(int *)data = ...`,
   which (a) silently corrupts adjacent struct bytes (`src_window_size`
   on `window_size`, padding before `heatmaps_path` on
   `max_log_contrast`), and (b) on `max_log_contrast` lands on a
   2-byte-aligned address — UBSan flags the misaligned 4-byte store. The
   mirror read site in `vmaf_feature_name_from_options`
   (`core/src/feature/feature_name.c:104`) shows the same misalignment
   on every CLI invocation that includes `--feature cambi`.

2. **AVX2 / AVX-512 ADM DWT2 filter packing left-shifts a negative `int`.**
   `core/src/feature/x86/adm_avx{2,512}.c` packs two adjacent 16-bit
   filter coefficients into a 32-bit lane via
   `_mm{256,512}_set1_epi32(filter[k] + (uint32_t)(filter[k+1] << 16))`.
   The cast on the *result* of the shift does not save the inner shift
   from being performed on a signed `int`; when `filter[k+1]` is negative
   (which it routinely is for the 9/7 DWT coefficients used in HBD
   inputs), the shift is C undefined behaviour. UBSan reports
   `left shift of negative value -4240` on every HBD ADM frame.

Neither bug surfaces in the existing unit tests because (a) `test_cambi`
does not parse user options via the CLI path, and (b) the AVX-512 ADM
fast path is only reached for HBD inputs sized to the SIMD step width,
which the unit-test fixtures do not exercise. Both ship in every release
binary and would eventually bite a CI run on a stricter UBSan profile
or a compiler that exploits the UB (clang/llvm's UB-driven optimiser is
already aggressive enough to silently miscompile these patterns on
`-O3`).

## Decision

We will treat both findings as real correctness bugs and fix them in
this PR, preserving bit-exact numerical behaviour on every input.

For (1), we introduce shadow `int` slots `window_size_opt` and
`max_log_contrast_opt` in `CambiState`. The options table targets the
shadow slots (preserving the option schema). `init()` copies them into
the existing `uint16_t` runtime fields — the option-parser bounds
(15..127 and 0..5 respectively) guarantee the narrowing is lossless.
This keeps the inner-loop signatures (`uint16_t window_size`, `uint16_t
max_log_contrast`) unchanged so no SIMD or scalar kernel needs to be
touched.

For (2), we move the `uint32_t` cast inside the shift's left operand:
`((uint32_t)filter[k+1] << 16)`. Shifting an unsigned operand is fully
defined and bit-exact with the original wrap-on-overflow behaviour on
every two's-complement target we ship for.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Introduce `VMAF_OPT_TYPE_UINT16` | Most type-safe; fixes a class of bugs in one stroke | New public option type touches every feature extractor, the dictionary serialiser, and the documentation | Larger blast radius than the bug warrants; only two fields in the entire tree had this mismatch |
| Change `window_size` / `max_log_contrast` to `int` outright | One-line struct change | Cascades into ~30 call sites that pass `&window_size` to `uint16_t *adjust_window_size`; SIMD kernels take `uint16_t window_size` and would need an explicit narrow at every call | More churn, more diff to audit, no behavioural difference |
| Shadow int + `init()` bridge (chosen) | One struct addition, one option-table line each, two assignments in init(); inner-loop signatures preserved; option schema preserved | Two shadow fields cost 8 bytes per `CambiState` instance | Minimal blast radius, surgical, easy to review |
| For (2), `// NOLINT` and ignore | Zero diff | Real C UB; the compiler is allowed to assume the shifted value is non-negative and miscompile downstream code | UB is not a style issue, and the fix is one cast |
| For (2), branch on the sign of `filter[k+1]` | Defensive | Hot path; branch is purely cosmetic given the cast already produces bit-exact output | Worse performance, no correctness gain |

## Consequences

- **Positive**: ASan + UBSan run clean across the entire unit-test suite
  (49 fast + 12 dnn + 2 slow = 63 tests, all OK), and CLI invocations
  exercising every CPU feature extractor across 4:2:0 8-bit, 4:2:2
  10-bit, and 4:2:0 12-bit inputs. The fork's CI now has a clean
  baseline for per-PR ASan/UBSan gating without grandfathered failures.
- **Positive**: A class of silent struct corruption (option-parser
  writing 4 bytes into a 2-byte field) is eliminated in CAMBI without
  touching the option-schema ABI.
- **Negative**: `CambiState` grows by 8 bytes per instance (negligible —
  CAMBI allocates multi-megabyte LUTs alongside the state).
- **Neutral / follow-up**: There is a wider pattern-class to audit
  (`BOOL` option written to an `int` field in
  `core/src/feature/float_adm.c::AdmState::adm_adm3_apply_hm`, and
  `unsigned`-typed fields targeted by `INT` options in `CambiState`
  for `enc_width` et al.). Neither triggers UBSan today and neither
  produces silent corruption on any platform we ship for (bool/int and
  int/unsigned share representation on every two's-complement target),
  but a follow-up audit should sweep them under a stricter option-type
  schema.

## References

- Source: agent dispatch — repo-level sanitizer audit on master tip
  `bbcaa8d127`.
- ADR-0278 (NOLINT citation closeout) — this PR adds no NOLINTs.
- `core/src/feature/cambi.c`, `core/src/feature/x86/adm_avx2.c`,
  `core/src/feature/x86/adm_avx512.c`.
- Related research digest: `docs/research/sanitizer-pass-2026-05-30.md`.
