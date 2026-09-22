<!-- markdownlint-disable MD013 -->

# Research digest — a HIP `init` unwind ladder that reported success (T-HIP-INIT-UNWIND-REPORTS-SUCCESS-2026-09-22)

- **Date**: 2026-09-22
- **Branch**: `integration/zero-warning-hiss21`
- **ADR**: [ADR-1296](../adr/1296-gpu-failure-path-stub-interposition.md)
- **Files**: `core/src/feature/hip/integer_adm_hip.c`, `core/src/feature/hip/ssimulacra2_hip.c`

## The defect in one paragraph

Three `init` failure branches across two HIP extractors released every resource
they had claimed and then returned `0`. The framework read that as success, set
`is_initialized`, and the next `submit` or `extract` ran against freed device
memory, unloaded modules and a destroyed stream. One of the three also skipped
two tiers of the unwind ladder, permanently leaking the two luma staging
buffers.

## Why a released state was reported as success

Each HIP extractor's unwind ladder is a chain of `static` helpers, one per tier,
each tail-calling the next-earlier tier. The chain terminates in a translator:

```c
/* core/src/feature/hip/integer_adm_hip.c */
static int adm_hip_unwind_stream(AdmStateHip *s, hipError_t rc)
{
    (void)hipStreamDestroy(s->str);
    return hip_rc(rc);           /* hip_rc(hipSuccess) == 0 */
}
```

`hip_rc()` — and its SSIMULACRA2 twin `ss2h_hip_rc()` — map `hipSuccess` to
`0`. Every legitimate caller passes a `hipError_t` it has just checked against
`hipSuccess`, so the ladder returns a real negative errno. Three call sites
passed a value that was `hipSuccess`:

| Site | Failure being unwound | What was passed |
| --- | --- | --- |
| `integer_adm_hip.c` `adm_hip_init_device()` | `vmaf_feature_name_dict_from_provided_features()` returned NULL — a **host** allocation, no `hipError_t` exists | literal `hipSuccess` |
| `ssimulacra2_hip.c` `init_fex_hip()` | `ss2h_alloc_device()` returned a negative errno | `hip_rc`, still holding the `hipSuccess` of the last `hipModuleGetFunction` |
| `ssimulacra2_hip.c` `init_fex_hip()` | `ss2h_alloc_pinned()` returned a negative errno | same |

The SSIMULACRA2 pair additionally discarded the allocator's own error code, so
even the shape of the failure was lost.

## Why the framework then used the released state

`core/src/feature/feature_extractor.cpp`:

```c
if (fex_ctx->fex->init && !fex_ctx->is_initialized) {
    const int err = fex_ctx->fex->init(fex_ctx->fex, pix_fmt, bpc, w, h);
    if (err)
        return err;
}
fex_ctx->is_initialized = true;
```

A `0` return is the only signal the framework has. `submit` and `extract` gate
on `is_initialized`, not on any per-backend liveness check, so the first frame
goes straight at the freed buffers.

## Why the skipped tiers were a leak and not a deferral

`vmaf_feature_extractor_context_close` opens with:

```c
if (!fex_ctx->is_initialized) return -EINVAL;
```

So `close_fex_hip` — which does free `d_ref_luma` and `d_dis_luma` — never runs
after a failed `init`. Any tier the ladder skips is lost for the process
lifetime. The in-tree comment on `adm_hip_unwind_buf_dev_to_host()` stated this
correctly and then described the leak as "tracked separately", which it was
not. Fixing the `hipSuccess` return without also fixing the skipped tiers would
have converted a use-after-free into a guaranteed device-memory leak, because
until the fix the path never returned an error at all and `close` therefore did
run.

## The provenance of the skip

Before HISS-01 removed `goto` from this file, the dictionary failure was a
`goto fail_host`, and the `fail_host:` label sat *below* `fail_ref_luma:`. The
label placement was the skip. HISS-01 reproduced it verbatim in
`adm_hip_unwind_buf_dev_to_host()` rather than fixing behaviour inside a
structural refactor — defensible as a refactor policy, but it then got written
into `core/src/feature/hip/AGENTS.md` as an invariant not to fix, and ADR-0759
threaded a new allocation (`buf_dev`) through the same skipping path without
revisiting it. Three passes over the same lines each preserved the defect
because the previous pass had documented it as intentional.

## The fix

`adm_hip_unwind_buf_dev()` is the correct entry point: `buf_dev` is the last
allocation `init` performs, and the ladder from there runs
`buf_dev → d_dis_luma → d_ref_luma → results_host → tmp_res → … → stream`, the
exact reverse of the allocation order. `adm_hip_unwind_dis_luma()`'s own comment
says it was slotted in for precisely that ordering.

```c
const int unwind_err = adm_hip_unwind_buf_dev(s, hipSuccess);
return (unwind_err != 0) ? unwind_err : -ENOMEM;
```

`-ENOMEM` matches all thirteen HIP siblings and every `core/src/feature/metal/*.mm`
twin. The errno is not routed through `hip_rc`, because there is no
`hipError_t` to translate — that routing is what produced the bug. The ladder's
return value is still captured and checked (`docs/principles.md`: a non-void
return is checked and *handled*; `(void)` is not handling), and a genuine HIP
error wins over `-ENOMEM`.

`ssimulacra2_hip.c` gets the same treatment through a named helper,
`ss2h_init_unwind_alloc()`, which forwards the allocator's own errno.
`ss2h_load_modules()` no longer needs its `hipError_t *rc_out` out-parameter,
which existed only to ferry that `hipSuccess` to the two call sites, so it is
gone. `adm_hip_unwind_buf_dev_to_host()` lost its only caller and is deleted —
an unused `static` is a `-Wunused-function` error under HISS-10.

## Sweep

Everything checked, and what it found:

| Scope | Result |
| --- | --- |
| The other 13 HIP extractors' dictionary paths | Clean — all return `-ENOMEM` |
| Every `core/src/feature/metal/*.mm` twin | Clean |
| Every other unwind call site in `integer_adm_hip.c` (lines 1430–1626) | Clean — each is guarded by `if (hip_err != hipSuccess)` |
| `hipError_t rc = hipSuccess` locals in `float_adm_hip.c`, `integer_ssim_hip.c`, `speed_chroma_hip.c`, `speed_temporal_hip.c`, `float_ssim_hip.c`, `integer_psnr_hip.c` | Clean — all are loop accumulators in allocation or launch helpers, never fed to an unwind tier |
| `ssimulacra2_hip.c` | **Two more instances of the same defect**; fixed here |
| `core/src/feature/cuda/integer_adm_cuda.c` | Clean — `adm_init_unwind()` ends `return -ENOMEM;` unconditionally, ignoring its `ret` argument |
| `core/src/feature/cuda/float_adm_cuda.c` | Clean — `float_adm_init_unwind()` likewise |
| `core/src/feature/sycl/integer_adm_sycl.cpp` | Clean — `close_fex_sycl(fex); return -ENOMEM;` |

## Regression gates

Two new `fast`-suite targets, neither of which needs a GPU (ADR-1296):
`core/test/test_hip_adm_init_unwind.c` and
`core/test/test_hip_ssimulacra2_init_unwind.c`. Both compile the extractor TU
against a complete set of stubs and assert two independent things — that `init`
reports a failure, and that every pointer the stubs handed out came back.

Verified red-then-green, one defect at a time:

| Reverted to | Failing assertion |
| --- | --- |
| `return adm_hip_unwind_host(s, hipSuccess);` (both defects) | "a failed feature-name dictionary must fail init, not report success" |
| `(void)adm_hip_unwind_host(s, hipSuccess); return -ENOMEM;` (defect 2 only) | "every device allocation must be released on the failed init path" |
| `return ss2h_init_unwind_mod_mul(s, hipSuccess);` | "a failed device allocation must fail init, not report success" |

## Reproducer

```bash
meson setup build-hip core -Denable_hip=true -Denable_hipcc=true \
    -Denable_cuda=false -Denable_sycl=false
ninja -C build-hip -j4
./build-hip/test/test_hip_adm_init_unwind
./build-hip/test/test_hip_ssimulacra2_init_unwind
```

## Loose ends found on the way, not fixed here

- `core/src/feature/hip/integer_adm_hip.c`'s `#include "common.h"` resolves to
  `core/src/cuda/common.h`. There is no `common.h` under `core/src/` or
  `core/src/feature/`, and the library build only compiles because
  `-I../core/src/cuda` is on the HIP TUs' command line. Any test target that
  compiles a HIP feature TU has to replicate that include directory. Worth
  disambiguating; out of scope for a bug fix.
- `make tidy-ratchet LANE=hip` does not complete on this workstation: three
  unrelated TUs (`core/src/libvmaf.c`, `core/test/test_feature_collector.c`,
  `core/test/test_flush_context_ordering.c`) fail clang-tidy's diagnostic
  parse. Tracked as `T-TIDY-RATCHET-GPU-LANES-UNREPRODUCIBLE-2026-09-22` in
  `docs/state.md`. The two touched TUs were measured individually with
  `--only` and sit exactly at their baseline allowances (7 and 2), so the
  baseline is unchanged.
