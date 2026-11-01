// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// build.rs — bindgen-based FFI binding generator for vmafx-sys.
//
// Environment variables:
//   LIBVMAF_PREFIX    First installation prefix, then /usr/local and /usr.
//   DOCS_RS           Use in-tree headers and omit native linking when set.
//   VMAFX_VENDOR_DIR  Override source checkout core/ path for vendored builds.
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
    println!("cargo:rerun-if-env-changed=VMAFX_VENDOR_DIR");
    println!("cargo:rerun-if-changed=build_support/header_check.rs");
    println!("cargo:rerun-if-changed=build_support/library_search.rs");

    let vendored = env::var_os("CARGO_FEATURE_VENDORED").is_some();
    let docs_rs = env::var_os("DOCS_RS").is_some();
    let prefix = env::var_os("LIBVMAF_PREFIX").map(PathBuf::from);
    let prefixes = library_search::search_prefixes(prefix.as_deref());
    let manifest = PathBuf::from(
        env::var_os("CARGO_MANIFEST_DIR").ok_or("vmafx-sys: CARGO_MANIFEST_DIR is not set")?,
    );
    // pkg-config is absent from Cargo.lock; keep the dependency-free fallback.
    let paths = library_search::find_paths(
        &manifest,
        &prefixes,
        docs_rs,
        vendored,
        Path::is_file,
        find_library,
        || build_vendored_core(&manifest),
    )?;

    if let Some(lib_dir) = &paths.lib_dir {
        match paths.link_kind {
            Some(library_search::LinkKind::Static) => {
                emit_static_links(lib_dir)?;
            }
            Some(library_search::LinkKind::Dylib) => {
                println!("cargo:rustc-link-search=native={}", lib_dir.display());
                println!("cargo:rustc-link-lib=dylib=vmaf");
                println!("cargo:rustc-link-lib=dylib=vmafx");
            }
            None => {}
        }
    }
    generate_bindings(&paths.include_dir)
}

fn emit_static_links(lib_dir: &Path) -> Result<(), Box<dyn std::error::Error>> {
    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    let static_libs = library_search::discover_static_libraries(lib_dir)?;
    for lib in &static_libs {
        println!("cargo:rustc-link-lib=static={lib}");
    }
    if let Some(build_dir) = lib_dir.parent() {
        let (extra_dirs, deps) = library_search::read_pc_dependencies(build_dir, pkg_config_libs)?;
        for dir in &extra_dirs {
            println!("cargo:rustc-link-search=native={}", dir.display());
            println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dir.display());
        }
        for dep in &deps {
            println!("cargo:rustc-link-lib={dep}");
        }
    }
    Ok(())
}

fn pkg_config_libs(module: &str) -> Result<String, String> {
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");
    let output = std::process::Command::new("pkg-config")
        .args(["--static", "--libs", module])
        .output()
        .map_err(|err| format!("vmafx-sys: failed to execute pkg-config for {module} ({err})"))?;
    if !output.status.success() {
        return Err(format!(
            "vmafx-sys: pkg-config cannot resolve {module}, which the vendored core links:\n{}",
            String::from_utf8_lossy(&output.stderr),
        ));
    }
    String::from_utf8(output.stdout)
        .map_err(|err| format!("vmafx-sys: pkg-config output for {module} is not UTF-8 ({err})"))
}

fn build_vendored_core(manifest: &Path) -> Result<library_search::BuildPaths, String> {
    let vendor_env = env::var_os("VMAFX_VENDOR_DIR").map(PathBuf::from);
    let source_dir =
        library_search::resolve_vendor_dir(vendor_env.as_deref(), manifest, Path::is_dir)?;
    println!("cargo:rerun-if-changed={}", source_dir.display());
    let out_dir = match env::var_os("OUT_DIR") {
        Some(val) => PathBuf::from(val),
        None => return Err("vmafx-sys: OUT_DIR is not set".into()),
    };
    let reconfigure = out_dir.join("build/build.ninja").is_file();
    let num_jobs = env::var("NUM_JOBS").ok();
    let paths = library_search::build_vendored(
        &source_dir,
        &out_dir,
        reconfigure,
        num_jobs.as_deref(),
        run_command,
    )?;
    if let Ok(version) = library_search::extract_core_version(&source_dir, &out_dir) {
        println!("cargo:rustc-env=VMAFX_CORE_VERSION={version}");
    }
    Ok(paths)
}

fn run_command(program: &str, args: &[&str]) -> Result<(), String> {
    let full_cmd = format!("{program} {}", args.join(" "));
    let mut cmd = std::process::Command::new(program);
    cmd.args(args);
    let output = match cmd.output() {
        Ok(output) => output,
        Err(err) => {
            return Err(format!(
                "vmafx-sys: failed to execute '{program}' ({err}); ensure {program} is installed and in PATH: command was '{full_cmd}'"
            ));
        }
    };
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let stdout = String::from_utf8_lossy(&output.stdout);
        return Err(format!(
            "vmafx-sys: {program} failed with exit status {status}: command was '{full_cmd}'\nstdout:\n{stdout}\nstderr:\n{stderr}",
            status = output.status,
        ));
    }
    Ok(())
}

fn has_library(dir: &Path, name: &str) -> bool {
    let candidates = [
        format!("lib{name}.so"),
        format!("lib{name}.a"),
        format!("lib{name}.dylib"),
        format!("{name}.lib"),
        format!("lib{name}.dll.a"),
    ];
    candidates
        .iter()
        .any(|candidate| dir.join(candidate).is_file())
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
    dirs.into_iter()
        .find(|dir| has_library(dir, "vmaf") && has_library(dir, "vmafx"))
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
