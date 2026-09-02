<!-- markdownlint-disable MD060 -->
# ADR-0533: Full HIP feature-extractor registration sweep

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `hip`, `gpu`, `feature-extractor`, `bugfix`, `fork-local`

## Context

ADR-0523 / PR #1283 fixed a single missing registration
(`vmaf_fex_integer_motion_hip`) and noted the bug pattern: a HIP
extractor TU declares a `VmafFeatureExtractor vmaf_fex_<name>_hip`
symbol and mirrors a CUDA twin call-graph-for-call-graph, but the TU is
never compiled into the HIP runtime archive and the symbol is never
declared in `core/src/feature/feature_extractor.c`. The result is
the registry lookup returns NULL and the extractor is dead code.

A repository-wide audit surfaced six more HIP TUs in the same state:

| Source                                                   | Symbol                            | Extractor name        |
|----------------------------------------------------------|-----------------------------------|-----------------------|
| `core/src/feature/hip/float_vif_hip.c`                | `vmaf_fex_float_vif_hip`          | `float_vif_hip`       |
| `core/src/feature/hip/integer_adm_hip.c`              | `vmaf_fex_integer_adm_hip`        | `adm_hip`             |
| `core/src/feature/hip/integer_ms_ssim_hip.c`          | `vmaf_fex_integer_ms_ssim_hip`    | `integer_ms_ssim_hip` |
| `core/src/feature/hip/integer_psnr_hvs_hip.c`         | `vmaf_fex_psnr_hvs_hip`           | `psnr_hvs_hip`        |
| `core/src/feature/hip/integer_ssim_hip.c`             | `vmaf_fex_integer_ssim_hip`       | `integer_ssim_hip`    |
| `core/src/feature/hip/ssimulacra2_hip.c`              | `vmaf_fex_ssimulacra2_hip`        | `ssimulacra2_hip`     |

Each TU already mirrors a long-shipped CUDA twin
(`float_vif_cuda` / `integer_adm_cuda` / `integer_ms_ssim_cuda` /
`psnr_hvs_cuda` / `integer_ssim_cuda` / `ssimulacra2_cuda`) and is
called out in the registry surface the CLI / `vmaf_use_feature` /
MCP `pick_features` tool consult. Without the registration both the
direct CLI invocation (`vmaf … --feature <name>`) and the dispatch
fall-back path fail at the name-lookup step before ever reaching the
backend's runtime check.

The three remaining `.c` files under `core/src/feature/hip/` —
`adm_hip.c`, `motion_hip.c`, `vif_hip.c` — are not affected. They
hold legacy plumbing stubs (`vmaf_hip_<metric>_run` helpers), not
`VmafFeatureExtractor` definitions, and are intentionally absent
from `hip_sources`. The two duplicate-named TUs
(`integer_ciede_hip.c`, `integer_moment_hip.c`) are stale rename
scaffolds — the canonical TUs (`ciede_hip.c`, `float_moment_hip.c`)
are already wired in; deduplication is out of scope here and tracked
separately.

## Decision

In a single sweep:

1. Add the six HIP source files to `hip_sources` in
   `core/src/hip/meson.build`.
2. Add an `extern VmafFeatureExtractor vmaf_fex_<symbol>;` declaration
   inside the `#if HAVE_HIP` block in
   `core/src/feature/feature_extractor.c` for each symbol.
3. Add a `&vmaf_fex_<symbol>,` entry to `feature_extractor_list[]`
   (also inside `#if HAVE_HIP`) immediately after
   `vmaf_fex_float_adm_hip` so the new rows extend the existing
   ADR-0523 region.
4. Pin the registration in `core/test/test_hip_smoke.c` with one
   `vmaf_get_feature_extractor_by_name(<name>) != NULL` assertion per
   newly-registered extractor.

Mirrors the PR #1283 / ADR-0523 pattern verbatim — same `extern`
location, same registry-entry style, same smoke-test shape.

## Alternatives considered

| Option                                                | Pros                                                 | Cons                                                                                | Why not chosen                                                  |
|-------------------------------------------------------|------------------------------------------------------|-------------------------------------------------------------------------------------|-----------------------------------------------------------------|
| **Single sweep (chosen)**                             | One ADR, one PR, one CI round-trip; fixes the bug class | Slightly larger diff than per-extractor                                             | —                                                               |
| One PR per extractor                                  | Smaller diffs                                        | Six PRs × CI overhead; the registration is mechanical                               | Rejected — PR-overhead memo (`pr_size_consolidation`) applies   |
| Delete the unregistered TUs                           | Reclaims build time                                  | Discards real ports that mirror CUDA twins; the kernels exist already               | Rejected — never weaken capability surface                      |
| Promote each TU to a full real kernel as part of this | End-state correctness                                | Each kernel is its own multi-week port + ADR; the registration bug stands on its own | Rejected — promotion already tracked per consumer (ADR-0254 etc.) |

## Consequences

- **Positive**: Six previously-dead extractors become discoverable
  by name. The CLI's `--feature <name>` plumbing, the
  `vmaf_use_feature` C-API path, and the MCP `pick_features`
  surface all see the HIP twins. Tests (`test_hip_smoke.c`)
  enforce the registration so a future drop-out is caught at
  pre-push time.
- **Negative**: The HIP archive grows by six TUs (`hip_sources`
  diff). Build time is unaffected in measurable terms — each TU
  is ~500–1500 lines of standard-C scaffolding that compiles in
  well under a second.
- **Neutral / follow-ups**: The scaffold contract is preserved.
  `init()` returns `-ENOSYS` on builds without
  `enable_hipcc=true` for the scaffold TUs (everything except
  the already-real `ssimulacra2_hip` / `integer_ms_ssim_hip`
  pair). Cross-backend numerical bit-exactness is not changed —
  the CPU dispatch fall-back already gates that.
- No rebase impact. The CUDA / SYCL / Vulkan / Metal twin
  registrations remain unchanged.

## References

- ADR-0523 — fixed the analogous single-extractor case
  (`vmaf_fex_integer_motion_hip`). This sweep generalises the same
  pattern to the rest of the HIP TUs.
- `core/src/feature/feature_extractor.c` lines 174–199 (extern
  block, post-edit), lines 322–337 (registry block, post-edit).
- `core/src/hip/meson.build` lines 107–127 — `hip_sources` sweep.
- `core/test/test_hip_smoke.c` — new per-extractor registration
  assertions.
- `req`: "Register ALL the HIP feature extractors in VMAFx/vmafx.
  PR #1283 only registered `vmaf_fex_integer_motion_hip` because
  the spec was too narrow — but `core/src/feature/hip/` has 20+
  kernel files that all need to land in `feature_extractor_list[]`."
