<!-- markdownlint-disable MD060 -->
# Research: Local Sanitizer Audit — 2026-05-30

## Summary

A local `-Db_sanitize=address,undefined` build of `core/` on master tip
`bbcaa8d127` was run against the full unit-test suite (49 fast + 12 dnn

- 2 slow = **63 tests, all OK**) plus the vmaf CLI binary against the
Netflix golden YUVs across 4:2:0 8-bit, 4:2:2 10-bit, and 4:2:0 12-bit
inputs and the full feature set.

Two real undefined-behaviour findings surfaced:

| # | Where | Class | Severity |
|---|-------|-------|----------|
| 1 | `core/src/feature/cambi.c` (`CambiState::window_size`, `CambiState::max_log_contrast`) | Misaligned store + silent struct-field overwrite via option-parser type mismatch | Real bug — every `--feature cambi` invocation; UB; silently clobbers `src_window_size` byte-pair. |
| 2 | `core/src/feature/x86/adm_avx{2,512}.c` (DWT2 filter packing) | C left-shift of negative signed `int` | Real bug — UB on every HBD ADM frame; defends against compiler exploitation. |

## Methodology

```bash
git worktree add -b fix/sanitizer-pass-cleanup /tmp/wt-sanitizer origin/master
cd /tmp/wt-sanitizer
meson setup build-asan core -Denable_cuda=false -Denable_sycl=false \
  -Db_sanitize=address,undefined
ninja -C build-asan

# Unit tests
ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:halt_on_error=0:print_stacktrace=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 \
meson test -C build-asan

# CLI exercise (Netflix golden fixtures, full feature set)
./build-asan/tools/vmaf \
  --reference  python/test/resource/yuv/src01_hrc00_576x324.yuv \
  --distorted  python/test/resource/yuv/src01_hrc01_576x324.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --feature psnr --feature float_ssim --feature float_ms_ssim \
  --feature ciede --feature cambi --feature psnr_hvs --feature vif

# HBD path (10-bit 4:2:2 + 12-bit 4:2:0)
./build-asan/tools/vmaf  ... 422p10le.yuv ... --pixel_format 422 --bitdepth 10 \
  --feature psnr --feature cambi --feature adm

# Prediction with model (leak check)
./build-asan/tools/vmaf ... --model "path=model/vmaf_v0.6.1.json"
```

ASan + UBSan + leak-check were enabled simultaneously across every run.

## Finding 1 — CAMBI option-parser misalignment + silent corruption

**Signal:**

```text
core/src/opt.c:46:10: runtime error: store to misaligned address
   0x7cc8bfde0902 for type 'int', which requires 4 byte alignment

core/src/feature/feature_name.c:104:38: runtime error: load of misaligned
   address 0x7cc8bfde0902 for type 'int', which requires 4 byte alignment
```

Trace lands in `vmaf_feature_extractor_context_create` →
`vmaf_fex_ctx_parse_options` and in `vmaf_feature_name_from_options`.

**Root cause:**

The options table in `core/src/feature/cambi.c` declares

```c
{ .name = "window_size",      .type = VMAF_OPT_TYPE_INT,
  .offset = offsetof(CambiState, window_size), ... }
{ .name = "max_log_contrast", .type = VMAF_OPT_TYPE_INT,
  .offset = offsetof(CambiState, max_log_contrast), ... }
```

but the underlying struct fields are `uint16_t`:

```c
typedef struct CambiState {
    ...
    uint16_t window_size;       // offset 212 — 4-byte aligned (lucky)
    uint16_t src_window_size;   //  -- clobbered by 4-byte int write
    ...
    uint16_t vlt_luma;
    uint16_t max_log_contrast;  // offset 258 — 2-byte aligned, NOT 4-byte
    char *heatmaps_path;
} CambiState;
```

`set_option_int` in `core/src/opt.c` writes `*(int *)data = value;`,
which:

- on `window_size`: writes 4 bytes starting at a 4-byte-aligned address,
  overwriting `src_window_size`. `init()` later sets `s->src_window_size
  = s->window_size`, so the corruption is masked at runtime — but the
  silent overwrite is real and an implementation tweak (e.g., reordering
  init) would expose it.
- on `max_log_contrast`: writes 4 bytes starting at a 2-byte-aligned
  address — UBSan's misaligned-access trap fires. The trailing 2 bytes
  spill into the padding before `heatmaps_path` (a pointer, 8-byte
  aligned), so the pointer itself survives — but that survival depends
  on the natural padding the compiler chose, not on any explicit
  contract.

The mirror is `vmaf_feature_name_from_options`
(`core/src/feature/feature_name.c:104`): the CLI's feature-name
derivation reads `*(int *)data` from the same offset on every frame.

**Fix:**

Add shadow `int` slots `window_size_opt` and `max_log_contrast_opt`,
point the options at them, and copy them into the existing `uint16_t`
runtime fields at the top of `init()`. The option-parser bounds (15..127
and 0..5 respectively) guarantee the narrowing is lossless. Inner-loop
SIMD/scalar signatures (`uint16_t window_size`) are unchanged, so no
extractor kernel needs to be touched.

**Verification:**

- Re-ran `--feature cambi` alone, `--feature cambi=window_size=63:max_log_contrast=3`,
  and the full 7-feature CLI command. All clean under UBSan.
- The feature-name derivation correctly produces `cambi_mlc_3_ws_63`
  for tuned options (proving the option-parser AND name generator both
  read the shadow slots correctly).
- Full unit-test suite (63 tests) passes; specifically, `test_cambi`,
  `test_cambi_simd`, `test_feature`, `test_feature_extractor`,
  `test_feature_collector`, `test_predict`, `test_model`,
  `test_model_collection_api`.
- CAMBI score values bit-exact with master on the Netflix golden YUV.

## Finding 2 — AVX2 / AVX-512 ADM signed-shift UB

**Signal:**

```text
core/src/feature/x86/adm_avx512.c:3558:66: runtime error:
   left shift of negative value -4240
```

Triggered on every HBD ADM frame.

**Root cause:**

```c
__m512i f23_lo = _mm512_set1_epi32(
    filter_lo[2] + (uint32_t)(filter_lo[3] << 16) /* + (1 << 16) */);
```

The cast on the *result* of the shift does not save the inner shift
from being performed on the original signed `int` type. C operator
precedence: the shift evaluates first on the signed type, then the
result is cast to `uint32_t`. When `filter_lo[3]` is negative (the
9/7-tap DWT filter has negative taps), the shift is undefined behaviour.

The fix is one character — move the cast inside:

```c
__m512i f23_lo = _mm512_set1_epi32(
    filter_lo[2] + ((uint32_t)filter_lo[3] << 16) /* + (1 << 16) */);
```

Shifting an unsigned operand is fully defined and produces the same bit
pattern as the original wrap-on-overflow signed shift on every
two's-complement target.

Same pattern appears 4× in `adm_avx512.c` and 4× in `adm_avx2.c`.

**Verification:**

- Re-ran HBD 4:2:2 10-bit and 4:2:0 12-bit CLI invocations exercising
  `--feature adm`. Clean under UBSan.
- `test_integer_adm_simd` (which compares AVX2/AVX-512 against scalar)
  still passes — bit-exact.
- Netflix golden 10-bit/12-bit ADM scores unchanged.

## Out-of-scope follow-up candidates

Surfaced by the same audit but **not** addressed in this PR (no UBSan
trigger today, no demonstrable miscompile risk on shipped targets):

1. `core/src/feature/cambi.c::CambiState::enc_width`, `enc_height`,
   `enc_bitdepth`, `src_width`, `src_height` — declared `unsigned`,
   targeted by `VMAF_OPT_TYPE_INT`. Layout-compatible on every ABI we
   ship for; UBSan does not flag.
2. `core/src/feature/float_adm.c::AdmState::adm_adm3_apply_hm` —
   declared `int`, targeted by `VMAF_OPT_TYPE_BOOL`. Safe by accident
   (priv memory is `memset(0)`d and `set_option_bool` writes the
   1-byte default first), but technically UB.

A future ADR can introduce a stricter option-type schema
(`VMAF_OPT_TYPE_UINT16`, etc.) to discharge the entire class. That
would be a much wider refactor and is out of scope here.

## Files touched

- `core/src/feature/cambi.c` — shadow `int` slots + init bridge
- `core/src/feature/x86/adm_avx2.c` — cast-inside-shift fix
- `core/src/feature/x86/adm_avx512.c` — cast-inside-shift fix
- `docs/adr/0869-sanitizer-pass-cleanup.md` — new ADR
- `docs/adr/README.md` — index row
- `changelog.d/fixed/sanitizer-pass-cleanup.md` — fragment
- `docs/research/sanitizer-pass-2026-05-30.md` — this digest
- `docs/rebase-notes.md` — entry
- `docs/state.md` — state-md update

## Reproduction command

```bash
git worktree add -b sanity-check /tmp/wt-check origin/master
cd /tmp/wt-check
meson setup build-asan core -Denable_cuda=false -Denable_sycl=false \
  -Db_sanitize=address,undefined
ninja -C build-asan
ASAN_OPTIONS=halt_on_error=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 \
  ./build-asan/tools/vmaf \
    --reference  python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted  python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --feature cambi
# master: emits UBSan misaligned-store warning + misaligned-load warning
# this PR: silent (no UBSan errors)
```
