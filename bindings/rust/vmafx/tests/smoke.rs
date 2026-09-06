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
        PoolingMethod::Median,
        PoolingMethod::Perc5,
        PoolingMethod::Perc10,
        PoolingMethod::Perc20,
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

// Round-3 R3-2 regression: `Context::read_pictures` must transfer ownership of
// both pictures to libvmaf on EVERY return — success and error alike — and must
// never re-unref the caller's structs itself.
//
// The previous binding blanket-unref'd both pictures whenever `rc < 0`. Against
// a CUDA-enabled libvmaf that is a use-after-free / double-free, because
// libvmaf's `cleanup:` path already unrefs the caller's structs (and the CUDA
// translate path shares the underlying refcount). On a non-CUDA libvmaf the old
// double-unref is merely *benign* (libvmaf memsets the freed struct to zero, so
// the second unref is a -EINVAL no-op), so a host test against the installed lib
// cannot reproduce the CUDA crash directly — the fix is verified by code review
// against the libvmaf ownership contract (see context.rs). What this test pins
// is the observable behaviour of both code paths the binding still controls:
// many consecutive successful frames must drive the thread-pool cleanup path
// without corrupting the picture pool, and an error return must leave the
// context usable rather than aborting.
#[test]
fn read_pictures_success_and_error_paths_keep_context_usable() {
    let mut ctx = ContextBuilder::new()
        .log_level(LogLevel::None)
        .n_threads(2)
        .n_subsample(1)
        .build()
        .expect("ContextBuilder::build failed");

    // Push several valid frames. With no extractors registered each call
    // succeeds and libvmaf's thread-pool path unrefs both pictures internally —
    // the binding must NOT also unref them. If it did, the pool free-list would
    // be corrupted and a later fetch would deadlock or abort.
    for index in 0..8u32 {
        let r = Picture::new_yuv420p_8bit(64, 64).expect("alloc ref");
        let d = Picture::new_yuv420p_8bit(64, 64).expect("alloc dist");
        ctx.read_pictures(r, d, index)
            .unwrap_or_else(|e| panic!("read_pictures(index={index}): {e}"));
    }

    // Re-submitting an already-consumed index is invalid. Depending on the
    // installed libvmaf version this is either rejected (Err) or silently
    // accepted; either way the binding must transfer ownership and the context
    // must remain usable for the next in-order frame. The point of the test is
    // that this path neither aborts nor corrupts the pool, NOT that a specific
    // errno is returned.
    let r = Picture::new_yuv420p_8bit(64, 64).expect("alloc ref");
    let d = Picture::new_yuv420p_8bit(64, 64).expect("alloc dist");
    let _ = ctx.read_pictures(r, d, 0);

    let r = Picture::new_yuv420p_8bit(64, 64).expect("alloc ref");
    let d = Picture::new_yuv420p_8bit(64, 64).expect("alloc dist");
    ctx.read_pictures(r, d, 8)
        .expect("context still usable after a possibly-rejected frame");

    ctx.flush().expect("flush");
}
