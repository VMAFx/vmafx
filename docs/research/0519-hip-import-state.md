# Research-0519: HIP `vmaf_hip_import_state` library-side wiring

**Status**: Investigation closed, fix landed (ADR-0519).
**Date**: 2026-05-18.
**Author**: Claude.

## Question

After ADR-0514 unblocked the container-side HIP exposure (added
`-DHAVE_HIP=1` to `core/tools/meson.build`), `vmaf --backend hip`
still failed with "problem during vmaf_hip_import_state" + exit 255
on an AMD gfx1036 host inside the `vmaf-dev-mcp` container. What is
the minimum library-side change to make HIP a fully working backend
on AMD?

## Method

1. Grep `vmaf_hip_import_state` across the tree. Found:
   - Declaration: `core/include/libvmaf/libvmaf_hip.h:80`.
   - Definition: `core/src/hip/common.c:149` returning `-ENOSYS`
     with a comment "stays unwired until the first feature kernel
     lands (T7-10c)".
   - Callers: `core/tools/vmaf.c:641` (the CLI `--backend hip`
     path), `core/test/test_hip_smoke.c:152` (explicit
     -ENOSYS assertion), `core/test/test_hip_motion3_parity.c:159`
     (uses the function but the test was never functional on master
     because it also depends on `vmaf_fex_integer_motion_hip` being
     registered, which it isn't).

2. Compare against the four other GPU backends' `_import_state`
   functions:
   - `vmaf_cuda_import_state` (`core/src/libvmaf.c:307`):
     by-value copy `vmaf->cuda.state = *cu_state;` — historic
     ownership-transfer semantics.
   - `vmaf_sycl_import_state` (`core/src/libvmaf.c:458`):
     pointer-stash `vmaf->sycl.state = sycl_state;` — caller-owned
     opaque state.
   - `vmaf_vulkan_import_state` (`core/src/libvmaf.c:571`):
     pointer-stash, identical shape.
   - `vmaf_metal_import_state` (`core/src/libvmaf.c:677`):
     pointer-stash, identical shape.

3. Check whether the HIP-flagged extractors actually need the
   stashed state. `core/src/hip/AGENTS.md` rebase-sensitive
   invariants section pins: "`vmaf_fex_psnr_hip` is registered
   without the `VMAF_FEATURE_EXTRACTOR_HIP` flag bit set
   (fork-local, ADR-0241). The flag bit (`1 << 6`) is reserved in
   the enum but the consumer does not set it, because the picture
   buffer-type check in `vmaf_feature_extractor_context_extract`
   would route a HIP-flagged extractor through a
   (not-yet-existing) HIP buffer-type branch." Every HIP feature
   extractor follows the same posture (grepped: 8 files cite the
   "VMAF_FEATURE_EXTRACTOR_HIP flag is intentionally absent" or
   "cleared until picture buffer-type" comment). The dispatch
   layer therefore routes HIP-named extractors through their CPU
   twins; the stashed state is captured for the future
   buffer-type plumbing, not for the current frame loop.

4. Confirm the end-to-end behaviour matches expectations: on the
   host build (where `vmaf_hip_import_state` was historically a
   silent no-op success), `vmaf --backend hip` produced VMAF =
   76.66783 — bit-exact equal to CPU. This is consistent with
   "HIP dispatch routes to CPU paths, returning CPU-identical
   scores", and is what the cross-backend e2e probe batch in
   `docs/state.md` recorded for the host build prior to the
   container regression.

## Findings

The minimum library-side change is three edits:

1. **Move `vmaf_hip_import_state` from `core/src/hip/common.c`
   into `core/src/libvmaf.c`.** The function needs
   `VmafContext` field-level access; placing it next to the CUDA
   / SYCL / Vulkan / Metal twins keeps the four "stash the
   borrowed state pointer on the context" implementations in one
   file where future maintainers can pattern-match across all of
   them.

2. **Add a `hip` substruct to `VmafContext`** holding a
   `VmafHipState *state` pointer, gated by `#ifdef HAVE_HIP`,
   appended after the existing `metal` substruct. Mirrors the
   SYCL / Vulkan / Metal field shapes exactly — the CUDA
   `VmafCudaCookie` + `VmafGpuPicturePool` ring-buffer fields are
   not needed yet because no HIP-flagged extractor consumes a
   HIP device picture today.

3. **Clear the pointer in `vmaf_close`** without freeing — the
   caller owns the state and frees it via `vmaf_hip_state_free`
   after `vmaf_close`. Same lifetime contract as SYCL / Vulkan
   / Metal; documented in the existing comments cloned from the
   Vulkan block.

The smoke test (`core/test/test_hip_smoke.c`) must replace its
`test_import_state_returns_enosys` assertion with two new ones:
NULL-argument validation (`-EINVAL`) and a device-bound happy-path
that exercises `vmaf_init` → `vmaf_hip_import_state` →
`vmaf_close` → `vmaf_hip_state_free` on a host with at least one
visible AMD GPU (skips cleanly otherwise, matching the
Vulkan-on-lavapipe-less-CI pattern).

## Verification

End-to-end repro inside `vmaf-dev-mcp`:

```text
vmaf --reference /workspace/python/test/resource/yuv/src01_hrc00_576x324.yuv \
     --distorted /workspace/python/test/resource/yuv/src01_hrc01_576x324.yuv \
     --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
     --backend hip --hip_device 0 --json --output /tmp/hip.json
```

Pre-fix: "problem during vmaf_hip_import_state" + exit 255.
Post-fix: VMAF = 76.66783, exit 0.

CPU baseline on the same pair: VMAF = 76.66783 (delta = 0, within
the `places=4` cross-backend gate from ADR-0214 with room to
spare).

Smoke test (`test_hip_smoke`): 22 / 22 pass (was 21 / 22 pass + 1
explicit -ENOSYS assertion that was the stop-gate).

## Out of scope

- Promoting `VMAF_FEATURE_EXTRACTOR_HIP` on the HIP-flagged
  extractors. Requires adding `VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE`
  to the picture buffer-type dispatch + a HIP picture pool — a
  separate, larger PR.
- Registering `vmaf_fex_integer_motion_hip` in
  `core/src/feature/feature_extractor.c`'s
  `feature_extractor_list[]`. The extractor is `extern`-declared
  but never added to the list, so `vmaf_use_feature("motion_hip", ...)`
  returns `-EINVAL`. This is a pre-existing latent bug from PR
  #1167 (the test_hip_motion3_parity gate was added in the same
  PR but the registration was missing). Unblocking that gate
  needs the registration row + a check of every other HIP
  extractor's registration status; the fix is mechanical but
  out of scope for the import-state PR.
- The `test_cli_parse_long_only_args` failure on HIP-enabled
  builds (`--threads abc` exits via a signal instead of exit(1)
  inside the test's `fork()` child). Pre-existing on any
  HIP-enabled build; reproduces on master without my changes
  once `-Denable_hip=true -Denable_hipcc=true` is set. Likely a
  HIP-runtime static-constructor interaction with the test's
  pipe-redirected stderr; needs its own investigation.

## References

- ADR-0519 — the resulting decision and rollout sequence.
- ADR-0514 — the predecessor that closed the container-side HIP
  exposure gap.
- ADR-0468 — first real HIP feature kernel (`float_adm_hip`); the
  predecessor PR whose existence invalidated the
  "stays unwired until the first feature kernel lands" comment.
- ADR-0212 — HIP backend scaffold + header-purity invariant.
- ADR-0214 — `places=4` (1e-4) cross-backend numeric gate.
- `docs/state.md` row `T-HIP-IMPORT-STATE-ENOSYS-2026-05-18` — the
  Open bug closed by this work.
