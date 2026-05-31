<!-- markdownlint-disable MD060 -->
# ADR-0519: Implement vmaf_hip_import_state to unblock --backend hip

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude
- **Tags**: hip, backend, libvmaf, gpu

## Context

ADR-0514 closed the container-side HIP exposure gap and added
`-DHAVE_HIP=1` to `core/tools/meson.build` so the CLI's
`#ifdef HAVE_HIP` blocks compile in. After ADR-0514 landed, the only
remaining gap blocking `vmaf --backend hip` was the library-side
state-binding stub: `vmaf_hip_import_state` in
`core/src/hip/common.c:149` returned `-ENOSYS` with the comment
"stays unwired until the first feature kernel lands". ADR-0468 had
since landed the first real HIP feature kernel (`float_adm_hip`), so
the rationale for the stub was stale; only the function body itself
remained.

The CLI's `--backend hip` path constructs a `VmafHipState`, then
calls `vmaf_hip_import_state(vmaf, hip_state)`. With the stub, the
call returned `-ENOSYS`, the CLI emitted "problem during
vmaf_hip_import_state" and aborted with exit 255 — even on a healthy
AMD gfx1036 host with ROCm 6.4 fully working (HIP state init, stream
create, device probe all succeeded; only the libvmaf-side bind was
missing). Tracked in `docs/state.md` as Open bug
`T-HIP-IMPORT-STATE-ENOSYS-2026-05-18`.

The fix surface is small. The CUDA / SYCL / Vulkan / Metal twins all
implement `_import_state` in `core/src/libvmaf.c` (next to the
`VmafContext` struct) as a thin "stash the borrowed state pointer on
the context, return 0" wrapper. The HIP variant should follow the
same pattern — the HIP feature extractors do not yet set the
`VMAF_FEATURE_EXTRACTOR_HIP` flag bit (every kernel registration
deliberately clears it, per the rebase-sensitive invariants in
`core/src/hip/AGENTS.md`), so dispatch routes them through their
CPU twins; the bound state is captured for the future
picture-buffer-type plumbing that flips the flag on.

## Decision

We will move `vmaf_hip_import_state` from `core/src/hip/common.c`
into `core/src/libvmaf.c` and implement it as a SYCL / Vulkan /
Metal-style "stash the borrowed state pointer on the VmafContext and
return 0" wrapper. The CUDA twin's by-value copy semantics
(`vmaf->cuda.state = *cu_state;`) are not the right precedent here
because the HIP backend, like SYCL / Vulkan / Metal, uses an opaque
`VmafHipState` pointer with caller-owned lifetime — the caller calls
`vmaf_hip_state_free()` after `vmaf_close()`. A new `hip` struct in
`VmafContext` carries the borrowed pointer under `#ifdef HAVE_HIP`,
and `vmaf_close()` clears the pointer without freeing the
underlying state.

The HIP smoke test (`core/test/test_hip_smoke.c`) updates to
verify the new contract: NULL arguments return `-EINVAL`, and the
device-bound happy path (skipped when no AMD GPU is visible)
covers the `vmaf_init` → `vmaf_hip_import_state` → `vmaf_close` →
`vmaf_hip_state_free` lifetime and the re-import idempotency.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Mirror the CUDA by-value copy (`vmaf->hip.state = *state;`) | Closest field-for-field parallel to the CUDA twin; obvious diff for future readers | Requires exposing the full `VmafHipState` struct definition to `libvmaf.c`, breaking the header-purity invariant the HIP backend pinned in ADR-0212 (`<hip/hip_runtime.h>` types stay on the implementation side); also adds an ownership-transfer ambiguity the SYCL / Vulkan / Metal twins deliberately avoid | Caller-owned pointer model matches the rest of the GPU backends; ADR-0212 header-purity invariant survives |
| Implement the function in `core/src/hip/common.c` and forward-declare a setter on `VmafContext` | Keeps every HIP-backend entry point in one TU | Inverts the existing pattern (CUDA / SYCL / Vulkan / Metal all live in `libvmaf.c`); future maintainers would not find the HIP variant where they expect it | Convention beats novelty for a 1-line wrapper |
| Wire `VMAF_FEATURE_EXTRACTOR_HIP` on the HIP-flagged extractors as part of the same PR | Unlocks the device-resident dispatch path simultaneously | Out of scope; the dispatch flip requires the picture buffer-type plumbing (`VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE`) per the AGENTS.md invariant — a separate, larger PR | Keep PR scope tight; the import-state fix alone unblocks `--backend hip` end-to-end |

## Consequences

- **Positive**: `vmaf --backend hip` produces a valid VMAF JSON on
  any AMD ROCm host. End-to-end verified on gfx1036 (AMD Radeon
  680M) inside the `vmaf-dev-mcp` container: VMAF = 76.66783,
  bit-exact match against the CPU backend (CPU = 76.66783, delta
  = 0). All other backends already meet the `places=4` (1e-4)
  cross-backend gate; HIP now joins them.
- **Positive**: closes the last Open bug from the post-ADR-0514
  container-backend-exposure investigation. `docs/state.md` row
  `T-HIP-IMPORT-STATE-ENOSYS-2026-05-18` moves to "Recently
  closed".
- **Negative**: the HIP-flagged extractors still do not have the
  `VMAF_FEATURE_EXTRACTOR_HIP` flag bit set, so the actual VMAF
  computation routes through the CPU twins. Scores match CPU
  exactly because they ARE the CPU paths — no device kernels run.
  Promoting the flag bit (and adding the
  `VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE` plumbing) is tracked
  separately; the current PR is the prerequisite, not the
  follow-up.
- **Neutral / follow-ups**: a future PR can register
  `vmaf_fex_integer_motion_hip` in `feature_extractor.c`'s
  feature list (currently declared `extern` but absent from
  `feature_extractor_list[]`, breaking the
  `test_hip_motion3_parity` gate added in PR #1167 that this PR
  does not unblock). That test currently fails at
  `vmaf_use_feature("motion_hip", NULL)` with `-EINVAL`,
  independent of the import-state fix.

## References

- Closes `docs/state.md` row `T-HIP-IMPORT-STATE-ENOSYS-2026-05-18`.
- Follows on from [ADR-0514](0514-dev-container-full-backend-exposure.md)
  which exposed the gap by closing every other layer of the
  `--backend hip` stack.
- Mirrors the SYCL / Vulkan / Metal `_import_state` pattern at
  `core/src/libvmaf.c:458` / `:571` / `:677`.
- [ADR-0212](0212-hip-backend-scaffold.md) — HIP scaffold + header
  purity invariant.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — `places=4` (1e-4)
  cross-backend numeric gate that HIP now joins.
- Source: `req` — user request 2026-05-18: "Fix the HIP library-side
  `vmaf_hip_import_state` ENOSYS in VMAFx/vmafx so HIP becomes a
  fully working backend on AMD GPUs (gfx1036)."
