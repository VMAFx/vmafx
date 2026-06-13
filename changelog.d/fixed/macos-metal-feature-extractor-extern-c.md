**fix(metal): hoist `feature_extractor.h` out of `extern "C"` in Metal `.mm` files**

Xcode 16.4 / macOS 15 (runner image `macos-15-arm64/20260527`) tightened
libc++ so that `<atomic>` — pulled in by `feature_extractor.h` via its
`#if defined(__cplusplus)` branch — emits a fatal "templates must have C++
linkage" error when encountered inside an `extern "C" {}` block.

All eight `core/src/feature/metal/*_metal.mm` files had `feature_extractor.h`
inside their opening `extern "C"` block. Moving it above that block (at
pure C++ file scope, where `__cplusplus` is defined and templates are legal)
restores the build on all macOS CI legs.

Affected files: `float_moment_metal.mm`, `float_motion_metal.mm`,
`float_ms_ssim_metal.mm`, `float_psnr_metal.mm`, `float_ssim_metal.mm`,
`integer_motion_metal.mm`, `integer_motion_v2_metal.mm`,
`integer_psnr_metal.mm`.
