# Research Digest 0687 — vmaf-tune score backend native priority

## Scope

Audit the `vmaf-tune --score-backend` selector against the current libvmaf CLI
backend surface and the operator rule that native GPU runtimes should win before
Vulkan when `auto` chooses a score backend.

## Findings

- `core/tools/cli_parse.c` already advertises
  `auto|cpu|cuda|sycl|vulkan|hip|metal` and configures `--backend hip` with
  `hip_device=0` plus the expected exclusive-backend disables.
- `docs/backends/hip/overview.md` records `vmaf --backend hip` as end-to-end
  working on ROCm hosts after the HIP import-state/runtime fixes.
- `tools/vmaf-tune/src/vmaftune/score_backend.py` still exposed only
  `cpu`, `cuda`, `sycl`, and `vulkan`, and its `auto` chain was
  `cuda -> vulkan -> sycl -> cpu`.
- The old order made sense when Vulkan was the only non-CUDA broad fallback.
  It is no longer the best default after the SYCL and HIP runtime work because
  native stacks avoid translation layers and match the current operator rule:
  CUDA first, then SYCL, then HIP/ROCm, then Vulkan, then CPU.

## Implementation Notes

- Add `hip` to the tune backend enum so argparse, command building, and strict
  `select_backend(prefer="hip")` work through the same path as CUDA/SYCL/Vulkan.
- Keep CPU in the accepted enum as the universal fallback, but not in the
  preferred GPU order until the end of the fallback chain.
- Probe HIP via `rocminfo` looking for a `gfx*` agent; if that is unavailable,
  fall back to `rocm-smi --showproductname`.
- Keep Vulkan in the chain after native runtimes for cross-vendor fallback and
  MoltenVK-style hosts.

## Validation Plan

- Unit-test parsing of a libvmaf help line that includes HIP and Metal; Metal is
  ignored until a dedicated probe ships.
- Unit-test `rocminfo` and `rocm-smi` HIP detection paths.
- Unit-test `auto` priority for SYCL-over-HIP-over-Vulkan.
- Unit-test explicit HIP strict success/failure.

## References

- [ADR-0667](../adr/0667-vmaf-tune-score-backend-native-priority.md)
- [docs/usage/vmaf-tune-score-backend.md](../usage/vmaf-tune-score-backend.md)
- [docs/backends/hip/overview.md](../backends/hip/overview.md)
