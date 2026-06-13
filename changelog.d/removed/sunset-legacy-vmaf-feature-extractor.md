# BREAKING: Remove VmafLegacyQualityRunner (float-path runner)

`VmafLegacyQualityRunner` has been removed from the Python harness.
The class depended on the `float_ansnr` feature extractor, which was
dropped from the C backend in PR #38. Any code importing or calling
`VmafLegacyQualityRunner` will receive an `ImportError`.

**Migration**: use `VmafQualityRunner` with a current VMAF model
(e.g. `vmaf_v0.6.1.json`). The modern runner uses the integer-path
feature extractors and is numerically equivalent to the legacy runner
for all content types.

ADR-0749.
