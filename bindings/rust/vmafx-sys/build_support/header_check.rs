// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// build_support/header_check.rs — refuse a libvmaf header older than the crates.
//
// `build.rs` binds whatever `libvmaf.h` sits under `LIBVMAF_PREFIX`. A header
// from an older install binds fine and only fails later, in the `vmafx` crate,
// as an unresolved import (issue #2786). The build script therefore checks the
// generated bindings for every libvmaf name the two crates use and stops with
// the header path and the missing names. `tests/header_check.rs` includes this
// file too: it plants a stale binding and holds the list to the names the
// crates use.

/// Every libvmaf name that `vmafx-sys/src/safe.rs` and `vmafx/src` use, as
/// bindgen spells it. `tests/header_check.rs` fails when a crate uses a bound
/// name that is missing here.
pub const REQUIRED_SYMBOLS: &[&str] = &[
    "VmafConfiguration",
    "VmafContext",
    "VmafLogLevel_VMAF_LOG_LEVEL_DEBUG",
    "VmafLogLevel_VMAF_LOG_LEVEL_ERROR",
    "VmafLogLevel_VMAF_LOG_LEVEL_INFO",
    "VmafLogLevel_VMAF_LOG_LEVEL_NONE",
    "VmafLogLevel_VMAF_LOG_LEVEL_WARNING",
    "VmafModel",
    "VmafModelConfig",
    "VmafPicture",
    "VmafPixelFormat_VMAF_PIX_FMT_YUV400P",
    "VmafPixelFormat_VMAF_PIX_FMT_YUV420P",
    "VmafPixelFormat_VMAF_PIX_FMT_YUV422P",
    "VmafPixelFormat_VMAF_PIX_FMT_YUV444P",
    "VmafPoolingMethod_VMAF_POOL_METHOD_HARMONIC_MEAN",
    "VmafPoolingMethod_VMAF_POOL_METHOD_MAX",
    "VmafPoolingMethod_VMAF_POOL_METHOD_MEAN",
    "VmafPoolingMethod_VMAF_POOL_METHOD_MEDIAN",
    "VmafPoolingMethod_VMAF_POOL_METHOD_MIN",
    "VmafPoolingMethod_VMAF_POOL_METHOD_PERC10",
    "VmafPoolingMethod_VMAF_POOL_METHOD_PERC20",
    "VmafPoolingMethod_VMAF_POOL_METHOD_PERC5",
    "vmaf_close",
    "vmaf_init",
    "vmaf_model_destroy",
    "vmaf_model_load_from_path",
    "vmaf_picture_alloc",
    "vmaf_picture_unref",
    "vmaf_read_pictures",
    "vmaf_score_pooled",
    "vmaf_use_features_from_model",
    "vmaf_version",
];

/// Splits Rust source into identifiers (ASCII letters, digits and `_`).
pub fn identifiers(source: &str) -> std::collections::BTreeSet<&str> {
    source
        .split(|c: char| !(c.is_ascii_alphanumeric() || c == '_'))
        .filter(|word| !word.is_empty())
        .collect()
}

/// The required names that the generated bindings do not define.
pub fn missing_symbols(bindings: &str) -> Vec<&'static str> {
    let present = identifiers(bindings);
    REQUIRED_SYMBOLS
        .iter()
        .copied()
        .filter(|name| !present.contains(name))
        .collect()
}

/// The build script's error for a header without the required names.
pub fn stale_header_message(header: &str, missing: &[&str]) -> String {
    format!(
        "libvmaf header {header} is older than vmafx-sys: it lacks {}. \
         Install the libvmaf of this tree (meson install of core/) and point \
         LIBVMAF_PREFIX at that prefix.",
        missing.join(", ")
    )
}
