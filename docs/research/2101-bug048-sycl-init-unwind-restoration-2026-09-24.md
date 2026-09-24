<!-- markdownlint-disable MD013 MD060 -->
# Research-2101 — BUG-048 section E SYCL init-unwind restoration (2026-09-24)

**Status:** Complete

**Authority inspected:** `origin/master` at `4e6916d16ac57647105d14a47a6680117d6b5738`

**Historical producer:** `709ce470e240efd0a43422cfb5f3ffafb857b065` (#157)

**Historical revert:** `5d070b0b4338da84a7c6d2169e0bfa782436d3cb`

**Scope:** extractor-owned SYCL USM, feature-name dictionaries, and graph registrations acquired by `init`; no score arithmetic or kernel dispatch changed.

## Finding

The historical fix was only partly present on current `master`. Four of its 16
translation units had since gained equivalent or stronger cleanup:

| Translation unit | Current cleanup that supersedes #157 |
|---|---|
| `integer_adm_sycl.cpp` | Calls its NULL-safe close path after allocation, LUT-upload, dictionary, and graph-registration failures |
| `integer_vif_sycl.cpp` | Resource and graph helpers close on every post-acquisition error |
| `integer_motion_sycl.cpp` | Closes allocation, UV, dictionary, and graph-registration failures |
| `integer_cambi_sycl.cpp` | Central `release_cambi_resources` runs for every init error |

The other 12 translation units still returned directly after acquiring one or
more resources. `integer_ssim_sycl.cpp` contains two descriptors, so the defect
covered 13 live extractor init functions:

- `float_adm_sycl.cpp`
- `float_motion_sycl.cpp`
- `float_psnr_sycl.cpp`
- `float_vif_sycl.cpp`
- `integer_ciede_sycl.cpp`
- `integer_moment_sycl.cpp`
- `integer_motion_v2_sycl.cpp`
- `integer_ms_ssim_sycl.cpp`
- `integer_psnr_hvs_sycl.cpp`
- `integer_psnr_sycl.cpp`
- `integer_ssim_sycl.cpp` (`float_ssim_sycl` and `integer_ssim_sycl`)
- `ssimulacra2_sycl.cpp`

The framework does not call an extractor's `close` callback when `init`
returns an error. A bare return therefore permanently leaked all successful
allocations before the failed allocation, or the entire allocation set when
feature-name dictionary construction failed. `float_moment_sycl` and
`psnr_sycl` also leaked their allocations and dictionary when graph
registration failed.

## Hypotheses and checks

| Rank | Hypothesis | Check | Result |
|---:|---|---|---|
| 1 | A later USM allocation failure leaks earlier allocations | Fail the second combined host/device allocation and count every returned pointer | Confirmed on the first affected descriptor: `float_adm_sycl` left 25 allocations live |
| 2 | Refactors already superseded part of the original fix | Trace every resource-acquiring return in all 16 historical TUs | Confirmed for the four TUs listed above |
| 3 | Dictionary construction failure leaks a fully allocated state | Return `NULL` from the wrapped dictionary builder | Confirmed on the 12 dictionary-bearing affected TUs before the repair |
| 4 | Graph-registration failure leaks after dictionary construction | Return `-EIO` from the wrapped graph registry | Confirmed for `float_moment_sycl` and `psnr_sycl` |
| 5 | Existing close callbacks are unsafe on partial state | Invoke the production close callback through every injected failure and tally double/missing frees | Falsified; every affected close callback NULL-guards its owned resources |

## Decision

Call each extractor's existing NULL-safe close callback before propagating any
post-acquisition error. This is the only-one-way bug fix: ownership already
lives in `close`, and duplicating a second manual unwind list beside it would
create immediate drift risk.

| Alternative | Decision | Reason |
|---|---|---|
| Reuse the existing close callback | Selected | One ownership list, already NULL-safe, and executable failure-injection proves partial-state safety |
| Add per-init manual free ladders | Rejected | Duplicates 5–26 pointer lists in every TU and can drift from normal close |
| Teach the framework to close after failed init | Rejected | Broad lifecycle semantic change; would double-close extractors that already self-unwind |
| Cherry-pick `709ce470e` | Rejected | Current TUs have materially diverged, four fixes are already superseded, and the historical patch contains unbraced-return mistakes in two files |

No ADR is needed: this restores documented ownership semantics and makes no
architectural, policy, public API, or numerical decision.

The local Praetor touched-file gate reports 12 unchanged, baselined HISS-04
findings in `float_adm_sycl.cpp`, `integer_ciede_sycl.cpp`, and
`ssimulacra2_sycl.cpp`. Several are the SYCL launch/lifecycle functions whose
inline ADR-0141 / ADR-0278 citations record that splitting them can defeat
device-kernel inlining. This mechanical error-path repair therefore uses
Praetor's recorded touched-debt-delta reason, the same conflict handling
already documented by `T-GPU-TIDY-LANE-BLIND-SPOT-2026-09-22`: the audit still
requires zero debt growth and passes at 276 active findings within the 276-item
baseline. No baseline or suppression changed.

## Device-free regression gate

`core/test/test_sycl_init_unwind.cpp` links the production static library with
GNU `--wrap` interposition for the two USM allocators, USM free, dictionary
construction/free, shared-frame setup, and graph register/unregister. It runs
without a SYCL device and checks:

1. failure of the second USM allocation after the first succeeded;
2. dictionary failure after every USM allocation succeeded;
3. graph-registration failure after the dictionary succeeded, where present;
4. zero outstanding allocations, no live dictionary, and balanced graph
   unregister on every failed init.

Link wrapping is deliberately enabled only for Linux static builds with LTO
disabled. LLVM LTO resolves the wrapped references before GNU ld can rewrite
them; this is the same constraint as `test_registration_partial_copy`.

### Red-cap evidence

Against unmodified `4e6916d16`, the gate failed immediately and deterministically:

```text
float_adm_sycl/second-allocation: rc=-12 expected=-12 allocations=25 outstanding=25
failed SYCL init must release every resource acquired by that init
```

### Green evidence

The same binary after the repair passes all allocation, dictionary, and graph
faults across all 13 affected descriptors:

```text
1/1 fast+sycl - libvmaf:test_sycl_init_unwind OK
```

Build configuration:

```bash
CC=icx CXX=icpx meson setup core/build-bug048-red core \
  -Denable_sycl=true -Dsycl_icpx_aot_targets= -Db_lto=false \
  -Ddefault_library=static -Denable_docs=false -Denable_tools=false \
  -Denable_asm=false -Dbuilt_in_models=false -Denable_dnn=disabled
meson compile -C core/build-bug048-red test_sycl_init_unwind
meson test -C core/build-bug048-red test_sycl_init_unwind --print-errorlogs
```

Netflix golden assertions were not touched. The change runs only on error
paths and cannot change a successfully initialized extractor's score.
