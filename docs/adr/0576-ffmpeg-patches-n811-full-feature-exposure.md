<!-- markdownlint-disable MD013 MD060 -->
# ADR-0576: ffmpeg-patches n8.1.1 full-feature-exposure sync

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `ffmpeg`, `build`, `ci`, `cuda`, `sycl`, `hip`, `vulkan`, `metal`

## Context

The `ffmpeg-patches/` stack targets FFmpeg n8.1.1. A comprehensive audit
was requested covering three goals:

**Goal A — base confirmation:** Verify the existing 13-patch series applies
cleanly to n8.1.1 (which it already listed as its target in `series.txt` and
`README.md`). A fresh clone + `git am --3way` run confirmed zero drift across
all 13 patches.

**Goal B — deleted-symbol drop:** The task brief mentioned potential dead
symbols from PR #1314 (Vulkan/Metal extractor TU deletion) and PR #1321
(HIP/CUDA dup-symbol cleanup). After auditing every patch hunk, no patch
references any `vmaf_fex_*` extractor symbol or `feature_hip.h`. The patches
only use the public C API layer (`vmaf_cuda_state_init`, `vmaf_sycl_state_init`,
etc.). The specific symbols mentioned in the brief — `float_ssim_cuda` —
remain live in the codebase (the symbol is defined in `integer_ssim_cuda.c`,
its file was not deleted). No symbol drops were needed.

**Goal C — feature exposure gap:** The `feature=name=<extractor>` passthrough
in the upstream filter already exposes the complete `feature_extractor_list[]`
— all ~80 registered extractors (CPU, CUDA, SYCL, HIP, Vulkan, Metal) are
reachable without a patch.  The genuine gap was the `VmafConfiguration`
fields `cpumask` and `gpumask` that have been present since libvmaf 3.0 but
were never exposed as FFmpeg filter AVOptions. This prevented users from
selecting SIMD ISA levels or disabling GPU dispatch paths from within ffmpeg.
`precision` (ADR-0119) and `no_reference` are CLI-only concerns with no C
API equivalent relevant to the filter.

## Decision

Add a new **patch 0014** that exposes `cpumask` and `gpumask` as `AV_OPT_TYPE_INT64`
AVOptions on all libvmaf filter variants (`libvmaf`, `libvmaf_sycl`,
`libvmaf_vulkan`, `libvmaf_metal`). Both options are wired into
`VmafConfiguration` at each `vmaf_init()` call site, including the upstream
`config_props_cuda` path which shares `LIBVMAFContext`. Default 0 for both
(all ISAs / GPU paths enabled), preserving existing behavior.

The patch is 30 lines of additions, zero deletions, and applies cleanly as
the fourteenth entry in the series. `series.txt` and `README.md` are updated
to document the new patch.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Skip — all extractors already reachable via `feature=` | No patch needed | `cpumask`/`gpumask` remain unexposed; SIMD regression testing from ffmpeg is impossible | Users cannot pin SIMD level or disable GPU paths without rebuilding libvmaf |
| Add separate patches per filter variant | Cleaner per-filter diffs | Four patches for one logical change; cumulative conflict surface grows | One patch covers the shared struct + all 5 init points cleanly |
| Expose as environment variable wrapper instead | No patch at all | Requires external shell glue; not composable with ffmpeg option system | AVOption is the standard FFmpeg mechanism |

## Consequences

- **Positive**: Users can now write `libvmaf=cpumask=8` to force AVX2-or-below
  dispatch, enabling SIMD regression testing within ffmpeg. `gpumask=1`
  disables CUDA without rebuilding. Symmetrical with the `--cpumask` /
  `--gpumask` flags on the libvmaf CLI.
- **Positive**: The full 14-patch series has been freshly verified against
  n8.1.1 with zero drift. Future series replay is now documented with an
  updated date in README.md.
- **Negative**: None — the change is purely additive with zero API breakage
  and backward-compatible default values.
- **Neutral**: `no_reference` and `precision` remain CLI-only and are not
  exposed through the filter; this is correct because neither maps to a
  `VmafConfiguration` field or filter-relevant C API surface.

## References

- `ffmpeg-patches/series.txt` — updated to include 0014.
- `ffmpeg-patches/README.md` — updated Contents + verification note.
- `ffmpeg-patches/0014-libvmaf-expose-cpumask-and-gpumask-on-all-vmaf-filte.patch`
- `core/include/libvmaf/libvmaf.h` — `VmafConfiguration.cpumask` / `.gpumask`
- Prior verification: ADR-0277 (2026-05-04), n8.1 → n8.1.1 bump (2026-05-09).
- req: "Full sync of the in-tree ffmpeg-patches/ stack: bump to latest stable FFmpeg + expose EVERY libvmaf feature/extractor/option that the patches currently don't expose."
