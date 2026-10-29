// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// build.rs — bindgen-based FFI binding generator for vmafx-sys.
//
// Environment variables:
//   LIBVMAF_PREFIX   First installation prefix, then /usr/local and /usr.
//   DOCS_RS          Use in-tree headers and omit native linking when set.
//
// A header that lacks a libvmaf name the crates use (an older install under
// the prefix) stops the build here with the header path and the missing names
// (issue #2786), instead of failing later in the `vmafx` crate.

use std::env;
use std::path::{Path, PathBuf};

#[path = "build_support/header_check.rs"]
mod header_check;
#[path = "build_support/library_search.rs"]
mod library_search;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("cargo:rerun-if-env-changed=LIBVMAF_PREFIX");
    println!("cargo:rerun-if-env-changed=DOCS_RS");
    println!("cargo:rerun-if-changed=build_support/header_check.rs");
    println!("cargo:rerun-if-changed=build_support/library_search.rs");
    let prefix = env::var_os("LIBVMAF_PREFIX").map(PathBuf::from);
    let prefixes = library_search::search_prefixes(prefix.as_deref());
    let manifest = PathBuf::from(
        env::var_os("CARGO_MANIFEST_DIR").ok_or("vmafx-sys: CARGO_MANIFEST_DIR is not set")?,
    );
    // pkg-config is absent from Cargo.lock; keep the dependency-free fallback.
    let paths = library_search::find_paths(
        &manifest,
        &prefixes,
        env::var_os("DOCS_RS").is_some(),
        Path::is_file,
        find_library,
    )?;
    if let Some(lib_dir) = &paths.lib_dir {
        println!("cargo:rustc-link-search=native={}", lib_dir.display());
        println!("cargo:rustc-link-lib=dylib=vmaf");
    }
    generate_bindings(&paths.include_dir)
}

/// Standard library directories, including distro multiarch subdirectories.
fn find_library(prefix: &Path) -> Option<PathBuf> {
    let mut dirs = vec![prefix.join("lib"), prefix.join("lib64")];
    if let Ok(entries) = std::fs::read_dir(prefix.join("lib")) {
        let mut children: Vec<_> = entries
            .filter_map(Result::ok)
            .map(|entry| entry.path())
            .filter(|path| path.is_dir())
            .take(256)
            .collect();
        children.sort();
        dirs.extend(children);
    }
    dirs.into_iter().find(|dir| {
        [
            "libvmaf.so",
            "libvmaf.a",
            "libvmaf.dylib",
            "vmaf.lib",
            "libvmaf.dll.a",
        ]
        .iter()
        .any(|name| dir.join(name).is_file())
    })
}

fn generate_bindings(include_dir: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let header = include_dir.join("libvmaf/libvmaf.h");
    let header = header.to_string_lossy();
    println!("cargo:rerun-if-changed={header}");

    let bindings = bindgen::Builder::default()
        .header(header.as_ref())
        .clang_arg(format!("-I{}", include_dir.display()))
        // Pull in transitive headers from the same include dir.
        .clang_arg(format!("-I{}", include_dir.join("libvmaf").display()))
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
