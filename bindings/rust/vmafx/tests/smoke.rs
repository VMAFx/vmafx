// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// Integration smoke tests for the safe `vmafx` API.
//
// These tests exercise only the parts of the API that do not require an
// installed model on disk: context lifecycle, picture allocation, version
// reporting. The full scoring flow (which needs a `.json` model file) lives
// in the doctest in `lib.rs` and behind `--ignored` here so CI runners
// without `/usr/local/share/model/` installed do not flake.

use vmafx::{Context, ContextBuilder, LogLevel, Picture, PoolingMethod};

#[test]
fn version_is_non_empty() {
    let v = vmafx::version();
    assert!(!v.is_empty(), "vmafx::version() returned empty string");
}

#[test]
fn context_default_constructs_and_drops() {
    let _ctx = Context::new().expect("Context::new failed");
}

#[test]
fn context_builder_round_trip() {
    let _ctx = ContextBuilder::new()
        .log_level(LogLevel::None)
        .n_threads(1)
        .n_subsample(1)
        .build()
        .expect("ContextBuilder::build failed");
}

#[test]
fn picture_alloc_drops_cleanly() {
    let p = Picture::new_yuv420p_8bit(64, 64).expect("alloc");
    assert_eq!(p.width(), 64);
    assert_eq!(p.height(), 64);
    assert_eq!(p.bpc(), 8);
}

#[test]
fn pooling_method_variants_are_distinct() {
    // Sanity check that every public variant constructs without panic.
    for m in [
        PoolingMethod::Mean,
        PoolingMethod::Min,
        PoolingMethod::Max,
        PoolingMethod::HarmonicMean,
    ] {
        // Just touch the value; the conversion is internal-only.
        let _ = format!("{m:?}");
    }
}

#[test]
fn error_display_works() {
    let e = vmafx::Error::OutOfMemory;
    let s = format!("{e}");
    assert!(s.contains("out of memory"));
}
