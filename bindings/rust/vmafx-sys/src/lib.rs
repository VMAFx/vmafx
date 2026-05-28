// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause
//
// vmafx-sys/src/lib.rs — raw FFI bindings to libvmaf.
//
// Generated at build time from the installed libvmaf public headers via bindgen.
// C symbols use C naming conventions; Rust lints for those are suppressed here.

#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

pub mod safe;
