<!-- markdownlint-disable MD013 MD041 MD060 -->

# Research digest: integer ADM upstream-mirror rework (ADR-1141)

- **Date**: 2026-09-02
- **Scope**: `core/src/feature/integer_adm.c` (3648 → 2292 LOC) and
  `core/src/feature/adm_tools.c` (1415 → 907 LOC), both Netflix-header
  upstream-mirror files
- **Decision record**: [ADR-1141](../adr/1141-integer-adm-upstream-mirror-rework.md)
- **Outcome**: both files lint-clean to the fork profile; every kernel
  expression verbatim; bit-exact on a 62-run CLI output matrix; fast
  suite green; goldens unchanged

## 1. Starting point

| Measure | `integer_adm.c` before | `adm_tools.c` before |
| --- | --- | --- |
| clang-tidy, as shipped | 6 (`modernize-use-nullptr`) | 65 |
| clang-tidy, NOLINT markers neutralised | 381 (335 `bugprone-macro-parentheses`, 22 `bugprone-implicit-widening-of-multiplication-result`, 15 `readability-function-size`, 6 `modernize-use-nullptr`, 2 `readability-non-const-parameter`, 1 `misc-use-internal-linkage`) | 65 (33 `readability-isolate-declaration`, 23 `bugprone-implicit-widening-of-multiplication-result`, 6 `readability-function-size`, 3 `misc-use-internal-linkage`) |
| cppcheck `--enable=all`, file-filtered, excluding `unusedFunction` artefacts | 31 (19 `constVariablePointer`, 6 `variableScope`, 4 `constVariable`, 2 `constParameterCallback`) | 35 (26 `variableScope`, 3 `constVariable`, 3 `constVariablePointer`, 3 `unreadVariable`) |
| NOLINT marker lines | 24 | 1 |
| Functions over the 60-line budget | 15 (`adm_cm` 291 lines, `i4_adm_cm` 330, `integer_compute_adm` 163, `adm_dwt2_s123_combined` 170, `init` 128, ...) | 6 (`adm_cm_s` 549 lines, `adm_decouple_s` 139, ...) |

The 335 `bugprone-macro-parentheses` findings all come from the twenty
upstream `ADM_CM_THRESH_S_*` / `I4_ADM_CM_THRESH_S_*` /
`ADM_CM_ACCUM_ROUND` / `I4_ADM_CM_ACCUM_ROUND` macros (490 lines) that
one `NOLINTBEGIN` bracket covered. Four `NOLINT(hicpp-signed-bitwise)`
markers suppressed a check the fork does not enable.

## 2. The macro collapse

Each of the nine threshold macros reads a 3x3 neighbourhood of the
CSF-filtered bands around `(i, j)` with hard-coded edge handling. Reading
the nine bodies side by side gives one rule:

| Variant | rows read | columns read |
| --- | --- | --- |
| `S_0_0` | 1, 0, 1 | 1, 0, 1 |
| `S_0_J` | 1, 0, 1 | j-1, j, j+1 |
| `S_0_W_M_1` | 1, 0, 1 | w-2, w-1, w-1 |
| `S_I_0` | i-1, i, i+1 | 1, 0, 1 |
| `S_I_J` | i-1, i, i+1 | j-1, j, j+1 |
| `S_I_W_M_1` | i-1, i, i+1 | w-2, w-1, w-1 |
| `S_H_M_1_0` | h-2, h-1, h-1 | 1, 0, 1 |
| `S_H_M_1_J` | h-2, h-1, h-1 | j-1, j, j+1 |
| `S_H_M_1_W_M_1` | h-2, h-1, h-1 | w-2, w-1, w-1 |

i.e. the index before the first edge mirrors to 1 and the index past the
last edge clamps to the last one; every body adds the nine terms in the
order (row-1: c-1, c, c+1), (row: c-1, centre, c+1), (row+1: c-1, c, c+1).
`adm_cm_thresh()` / `i4_adm_cm_thresh()` (and `adm_cm_thresh3x3_s()` for
the float header macros) compute `i_m1 = i == 0 ? 1 : i - 1`,
`i_p1 = i == h - 1 ? h - 1 : i + 1` (same for `j`) and add in that order.
The call sites passed exactly the `(i, j)` each macro was specialised for
(`0`, `start_col..end_col-1`, `w-1`, ...), so the closed form selects the
same terms; for the integer twins the sum is order-free, for the float
twin the order is preserved. The four-way border branch of `adm_cm` /
`i4_adm_cm` / `adm_cm_s` reduces to the two predicates it tested
(`left <= 0`, `right > w - 1`); `right = w - left` makes two of the four
arms unreachable, which is why the `rfactor[i * src_stride + w - 1]`
out-of-bounds read in the third arm of `i4_adm_cm` never fired.

The remaining moves are listed function by function in
[`docs/rebase-notes.md`](../rebase-notes.md) ("refactor/c-rework-adm").

## 3. Bit-exactness method

The Netflix golden pairs only reach the interior branch (at 576x324 and
1080p every scale keeps `left > 0`). The proof therefore diffs the full
CLI output of a baseline binary (master `6280fd2e2`, its `libvmaf.so`
preserved) against the refactored library across a matrix that reaches
every code path the CLI can select, each case run with SIMD dispatch on
(`--cpumask 0`) and scalar-only (`--cpumask 4294967295`), all at
`--precision max` (`%.17g`):

- the three Netflix pairs with `--model version=vmaf_v0.6.1`;
- `float_adm` (with `debug=true`) on `src01` and the 1-px checkerboard;
- the 10-bit `src01` pair (`integer` + `float_adm`);
- `adm` option variants: `adm_skip_scale0`, `adm_skip_aim`,
  `adm_csf_mode=1/2/3` with scaled CSF weights, `adm_norm_view_dist=4.0`
  with `adm_ref_display_height=2160`, and `adm_enhn_gain_limit=1.0` with
  `adm_p_norm=2.5`, `adm_noise_weight=0.1`, `adm_dlm_weight` and
  `adm_min_val`; `float_adm` variants of the same;
- deterministic synthetic 8-bit and 10-bit frames of 18x20, 34x34,
  40x24, 64x48 (2 frames, seeded noise), with and without
  `adm_p_norm=2.0` / `adm_csf_mode=1`. Their scales 1..3 are 2..9 wide, so
  `left <= 0` / `right > w - 1` fire and the corner / edge threshold paths
  and the `shift == 0` rounding terms run. (17x17 and 25x19 were also
  generated but the CLI rejects odd 4:2:0 dimensions — identically in both
  binaries.)

`compare-matrix.py` drops the volatile `fps` key and requires the parsed
JSON documents to be equal: **62 files, 21 396 per-frame metric values,
0 differences**, both before and after the final lint-driven edits. The
three golden means from the refactored build (identical to the baseline
binary on this host; the residuals against the upstream-published values
are the host libm and sit far inside `places=4`):

| Pair | `integer_adm2` mean | published golden | delta |
| --- | --- | --- | --- |
| `src01_hrc00` / `src01_hrc01` 576x324 | `0.9345057732668366` | `0.9345149030293786` | 9.1e-06 |
| checkerboard 0_0 / 1_0 1920x1080 | `0.7853317816736837` | `0.7853384465157921` | 6.7e-06 |
| checkerboard 0_0 / 10_0 1920x1080 | `0.053993285142625795` | `0.053996580527295335` | 3.3e-06 |

`meson test -C build --suite=fast` (the SIMD-twin gate, including
`test_integer_adm_simd`, `test_adm_coverage`, `test_integer_adm_min_dim`):
105 Ok, 0 Fail.

## 4. After

| Measure | `integer_adm.c` | `adm_tools.c` |
| --- | --- | --- |
| clang-tidy (`clang-tidy -p build`) | 0 | 0 |
| cppcheck `--enable=all`, file-filtered, excluding `unusedFunction` artefacts | 0 | 0 |
| NOLINT marker lines | 5 (one `modernize-use-nullptr` bracket, two `readability-non-const-parameter`, one `misc-use-internal-linkage`) | 1 (`readability-function-size` on `adm_dwt2_s`, ADR-1057) |
| cppcheck inline suppressions | 3 `constParameterCallback` (two decouple `lut`, `extract` pictures) | 0 |
| Functions over the 60-line budget | 0 | 1 (cited) |
| `pre-commit run --files` | all hooks pass (clang-format, semgrep, copyright, ...) | same |

## 5. Latent defects found on the way

- `adm_cm` / `i4_adm_cm`: `add_shift_xhcub = (uint32_t)pow(2, shift - 1)`
  with `uint32_t shift == 0` (pictures 17..32 wide at scale 0, or any
  scale narrow enough) computes `pow(2, UINT32_MAX)` = +Inf and casts it —
  undefined; x86-64 GCC produces 0. `adm_half_shift()` guards it
  (`adm_csf_den_s123` and `i4_adm_cm` already guarded one of their shifts)
  and yields the same 0. The synthetic 18x20 .. 64x48 cases exercise it.
- `i4_adm_cm`'s "left border within frame, right outside" arm read
  `rfactor[i * src_stride + w - 1]` from a 3-element array; unreachable,
  now gone with the arm.
- `adm_dwt2_lo_d` and `adm_buffer_copy` in `adm_tools.c` had no caller in
  the tree and no declaration in the header; removed. `adm_dwt2_d` has no
  C caller either but is bound by `compat/python-vmaf/core/adm_dwt2_cy.pyx`
  and stays.
- `ADM_CM_THRESH_S_*` in `adm_tools.h` are no longer used by any TU
  (follow-up: drop them from the header).

## 6. Deliberately not done

- The SIMD twins `x86/adm_avx2.c` / `x86/adm_avx512.c` keep their own
  macro copies; the dispatch prototypes (`int32_t *adm_div_lookup`,
  `AdmBuffer *buf`) are untouched, hence the cited suppressions.
- `adm_dwt2_s` is not split (ADR-1057 fp-contract bracket, ARM-only
  observability).
- The ADR-0155 rounding term and the `int32_t` narrowings of the decouple
  stage are upstream arithmetic encoded in the goldens; untouched.

## 7. Reproduce

```bash
meson setup build core -Denable_cuda=false -Denable_sycl=false && ninja -C build
meson test -C build --suite=fast --print-errorlogs
clang-tidy -p build core/src/feature/integer_adm.c core/src/feature/adm_tools.c   # no output
Y=python/test/resource/yuv
build/tools/vmaf --reference $Y/src01_hrc00_576x324.yuv --distorted $Y/src01_hrc01_576x324.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 --model version=vmaf_v0.6.1 \
  --cpumask 4294967295 --precision max --json --output /dev/stdout \
  | python3 -c "import json,sys; print(json.load(sys.stdin)['pooled_metrics']['integer_adm2']['mean'])"
# 0.9345057732668366 — identical with --cpumask 0 and with the pre-refactor binary
```
