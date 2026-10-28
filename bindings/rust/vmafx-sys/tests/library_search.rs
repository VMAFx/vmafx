// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

#[path = "../build_support/library_search.rs"]
mod library_search;

use library_search::{find_paths, search_prefixes};
use std::path::{Path, PathBuf};

#[test]
fn prefix_precedes_system_paths() {
    assert_eq!(
        search_prefixes(Some(Path::new("/custom"))),
        ["/custom", "/usr/local", "/usr"].map(PathBuf::from)
    );
}

#[test]
fn defaults_are_unique_and_empty_prefix_is_ignored() {
    let defaults = ["/usr/local", "/usr"].map(PathBuf::from);
    assert_eq!(search_prefixes(None), defaults);
    assert_eq!(search_prefixes(Some(Path::new(""))), defaults);
    assert_eq!(search_prefixes(Some(Path::new("/usr/local"))), defaults);
    assert_eq!(
        search_prefixes(Some(Path::new("/usr"))),
        ["/usr", "/usr/local"].map(PathBuf::from)
    );
}

#[test]
fn complete_prefix_wins() -> Result<(), String> {
    let paths = find_paths(
        Path::new("/repo/bindings/rust/vmafx-sys"),
        &search_prefixes(Some(Path::new("/custom"))),
        false,
        |path| path == Path::new("/custom/include/libvmaf/libvmaf.h"),
        |prefix| Some(prefix.join("lib")),
    )?;
    assert_eq!(paths.include_dir, Path::new("/custom/include"));
    assert_eq!(paths.lib_dir, Some(PathBuf::from("/custom/lib")));
    Ok(())
}

#[test]
fn missing_install_has_named_error_and_ordered_paths() {
    let result = find_paths(
        Path::new("/repo/bindings/rust/vmafx-sys"),
        &search_prefixes(Some(Path::new("/missing"))),
        false,
        |_| false,
        |_| None,
    );
    assert_eq!(result, Err("vmafx-sys: libvmaf not found (looked in: /missing, /usr/local, /usr); set LIBVMAF_PREFIX or install libvmaf development files".into()));
}

#[test]
fn header_without_library_is_not_an_install() {
    let result = find_paths(
        Path::new("/repo/bindings/rust/vmafx-sys"),
        &search_prefixes(None),
        false,
        |_| true,
        |_| None,
    );
    assert!(result.is_err());
}

#[test]
fn incomplete_prefix_falls_back_to_complete_system_install() -> Result<(), String> {
    let paths = find_paths(
        Path::new("/repo/bindings/rust/vmafx-sys"),
        &search_prefixes(Some(Path::new("/incomplete"))),
        false,
        |_| true,
        |prefix| (prefix == Path::new("/usr")).then(|| prefix.join("lib")),
    )?;
    assert_eq!(paths.include_dir, Path::new("/usr/include"));
    assert_eq!(paths.lib_dir, Some(PathBuf::from("/usr/lib")));
    Ok(())
}

#[test]
fn docs_rs_uses_tree_header_and_never_probes_libraries() -> Result<(), String> {
    let manifest = Path::new("/repo/bindings/rust/vmafx-sys");
    let include = manifest.join("../../../core/include");
    let mut probes = 0;
    let paths = find_paths(
        manifest,
        &search_prefixes(Some(Path::new("/ignored"))),
        true,
        |path| path == include.join("libvmaf/libvmaf.h"),
        |_| {
            probes += 1;
            None
        },
    )?;
    assert_eq!(paths.include_dir, include);
    assert_eq!(paths.lib_dir, None);
    assert_eq!(probes, 0);
    Ok(())
}

#[test]
fn docs_rs_refuses_missing_tree_header_without_system_fallback() {
    let manifest = Path::new("/repo/bindings/rust/vmafx-sys");
    let result = find_paths(manifest, &search_prefixes(None), true, |_| false, |_| None);
    assert_eq!(
        result,
        Err(format!(
            "vmafx-sys: DOCS_RS header not found: {}; use a source checkout with core/include/libvmaf/",
            manifest
                .join("../../../core/include/libvmaf/libvmaf.h")
                .display()
        ))
    );
}
