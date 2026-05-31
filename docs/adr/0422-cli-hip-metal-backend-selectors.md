<!-- markdownlint-disable MD056 MD060 -->
# ADR-0422: CLI HIP and Metal Backend Selectors

- **Status**: Accepted
- **Date**: 2026-05-11
- **Deciders**: Kilian, Claude (Anthropic)
- **Tags**: `cli`, `hip`, `metal`, `gpu`, `fork-local`

## Context

The Metal backend (T8-1b, ADR-0420/ADR-0421) and the HIP backend (ADR-0212
scaffold) are fully wired at the engine level — feature extractors are
registered under `HAVE_METAL` / `HAVE_HIP` guards and imported into
`VmafContext` via `vmaf_metal_import_state` / `vmaf_hip_import_state`. However,
the standalone `vmaf` CLI (`core/tools/`) lacked the surface flags to
activate either backend at runtime:

- No `--no_hip` / `--hip_device` flags existed (HIP shipped with engine wiring
  but no CLI counterparts).
- No `--no_metal` / `--metal_device` flags existed.
- `--backend` did not accept `hip` or `metal` as values.

This meant a user could not select HIP or Metal via the CLI even on a binary
built with `-Denable_hip=true` or (on macOS) with Metal enabled. The gap was
discovered during a post-merge audit of the Metal runtime PR (#765).

The fix is symmetric: add the four flag pairs and two `--backend` values
following the established Vulkan/SYCL pattern (`X_device >= 0` as the
activation trigger; `--backend X` disables all siblings and defaults device
to 0).

## Decision

Add `--no_hip`, `--hip_device <N>`, `--no_metal`, `--metal_device <N>` to the
CLI argument parser (`CLISettings`, `ARG_*` enum, `long_opts[]`, switch cases,
usage string), extend `--backend` to accept `hip` and `metal`, and add the
corresponding `vmaf_hip_state_init` / `vmaf_metal_state_init` blocks to
`init_gpu_backends()` in `vmaf.c`. Activation follows the Vulkan opt-in model:
the device flag must be non-negative (explicitly set) for the backend to engage;
`--backend hip|metal` defaults the device index to 0 and disables all other
backends.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Activate HIP/Metal via `--gpumask` like CUDA | Consistent with CUDA | `gpumask` is a CUDA-specific disable bitmask; reusing it for AMD/Apple semantics is confusing and conflicts with the CUDA-disable bit contract | Rejected |
| Auto-activate when built in (no explicit flag required) | Less user friction | Silent GPU use surprises users on multi-backend Linux hosts; breaks the opt-in contract SYCL/Vulkan/HIP already established | Rejected |
| Ship only `--backend hip|metal`, skip per-flag pairs | Smaller diff | Users cannot combine `--no_hip --no_metal` on a fully-built binary to force CPU; breaks the granular-disable pattern used by CUDA/SYCL/Vulkan | Rejected |

## Consequences

- **Positive**: HIP and Metal are now fully CLI-accessible; `--backend hip` /
  `--backend metal` work analogously to `--backend vulkan`; `--backend cpu`
  now correctly disables all five GPU backends.
- **Positive**: Test coverage added (`test_backend_hip`, `test_backend_metal`,
  `test_hip_device_explicit`, `test_metal_device_explicit`,
  `test_no_hip_no_metal_flags` in `core/test/test_cli_parse.c`).
- **Neutral**: `docs/usage/cli.md` updated; no ffmpeg-patches update required
  (the patches consume `libvmaf` C API / public headers, not the `vmaf` CLI
  tool flags).
- **Negative**: None identified.

## References

- ADR-0420: Metal backend runtime (T8-1b).
- ADR-0421: Metal first kernel (`motion_v2`).
- ADR-0212 placeholder: HIP scaffold.
- PR #765: Metal test-link fix + ffmpeg-patch 0012 repair (merged, master
  `5cb295ec`).
- User direction: the user requested HIP and Metal CLI parity after the audit
  revealed both backends were wired at engine level but not exposed via the CLI.
