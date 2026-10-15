// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// build.rs — bindgen-based FFI binding generator for vmafx-sys.
//
// Environment variables:
//   LIBVMAF_PREFIX   Installation prefix for libvmaf (default: /usr/local).
//                    The build script expects:
//                      ${LIBVMAF_PREFIX}/include/libvmaf/libvmaf.h
//                      ${LIBVMAF_PREFIX}/lib/libvmaf.so  (or .a)
//
// A header that lacks a libvmaf name the crates use (an older install under
// the prefix) stops the build here with the header path and the missing names
// (issue #2786), instead of failing later in the `vmafx` crate.

use std::env;
use std::path::PathBuf;

#[path = "build_support/header_check.rs"]
mod header_check;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let prefix = env::var("LIBVMAF_PREFIX").unwrap_or_else(|_| "/usr/local".to_string());
    let include_dir = format!("{prefix}/include");
    let lib_dir = format!("{prefix}/lib");

    // Tell cargo to link against libvmaf.
    println!("cargo:rustc-link-search=native={lib_dir}");
    println!("cargo:rustc-link-lib=dylib=vmaf");

    // Re-run the build script if the header changes.
    let header = format!("{include_dir}/libvmaf/libvmaf.h");
    println!("cargo:rerun-if-changed={header}");
    println!("cargo:rerun-if-env-changed=LIBVMAF_PREFIX");
    println!("cargo:rerun-if-changed=build_support/header_check.rs");

    let bindings = bindgen::Builder::default()
        .header(&header)
        .clang_arg(format!("-I{include_dir}"))
        // Pull in transitive headers from the same include dir.
        .clang_arg(format!("-I{include_dir}/libvmaf"))
        // Allowlist only the libvmaf public surface; skip OS internals.
        .allowlist_function("vmaf_.*")
        .allowlist_type("Vmaf.*")
        .allowlist_var("VMAF_.*")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()?;

    let missing = header_check::missing_symbols(&bindings.to_string());
    if !missing.is_empty() {
        return Err(header_check::stale_header_message(&header, &missing).into());
    }

    let out_path = PathBuf::from(env::var("OUT_DIR")?);
    bindings.write_to_file(out_path.join("bindings.rs"))?;
    Ok(())
}
