// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause
//
// vmafx-sys/src/lib.rs — raw FFI bindings to libvmaf.
//
// The low-level bindgen output lives inside the `bindings` module, which carries
// blanket lint suppressions appropriate for machine-generated C-naming FFI code.
// Hand-written modules (`safe`) carry their own, stricter lint profiles and are
// NOT covered by the generated-bindings suppressions.

// Re-export everything from the generated bindings at the crate root so
// existing callers continue to work without a path change.
#[allow(
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    clippy::all
)]
mod bindings {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

pub use bindings::*;

pub mod safe;
