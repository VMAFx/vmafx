<!-- markdownlint-disable MD013 MD060 -->
# ANSNR (removed)

!!! warning "This metric was removed"
    The `float_ansnr` feature extractor was removed from every backend in
    PR #38 (see [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md)
    and the follow-up
    [ADR-0749](../adr/0749-sunset-legacy-vmaf-feature-extractor.md)).
    ANSNR is a pre-VMAF metric (2001) that Netflix never adopted in
    production; it was carried by the upstream codebase but no shipped VMAF
    model references it. This page is retained for traceability.

## Historical description

ANSNR (Average Noise-to-Signal Ratio) measured the ratio of distortion
energy to signal energy averaged over the frame. It was registered as the
`float_ansnr` feature extractor and emitted `float_ansnr` (luma) plus
`float_ansnr_cb` / `float_ansnr_cr` when `enable_chroma=true`.

## Migration

| Old surface | Status after PR #38 |
|---|---|
| `--feature float_ansnr` on the CLI | Unknown feature; the CLI errors out. |
| `_METRIC_TO_EXTRACTOR["float_ansnr"]` (Python helper in `ai/data/feature_extractor.py`) | Mapping entry removed; passing `features=["float_ansnr"]` raises. |
| `float_ansnr_hip` / `float_ansnr_cuda` / `float_ansnr_sycl` (GPU twins) | Sources removed together with the CPU registration. |
| `VmafLegacyQualityRunner` (Python runner that fed ANSNR into a libsvm model) | Sunset in [ADR-0749](../adr/0749-sunset-legacy-vmaf-feature-extractor.md). Use `VmafQualityRunner` against a modern `.json` model instead. |

Callers that need a per-frame distortion-energy signal should use PSNR
(`psnr_y` / `psnr_cb` / `psnr_cr`) or PSNR-HVS (`psnr_hvs`); both remain
first-class extractors with active CPU + GPU paths.

## See also

- [Features](features.md) — current feature extractor reference.
- [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md)
- [ADR-0749](../adr/0749-sunset-legacy-vmaf-feature-extractor.md)
