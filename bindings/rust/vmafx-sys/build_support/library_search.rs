// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

use std::path::{Path, PathBuf};

#[derive(Debug, PartialEq, Eq)]
pub struct BuildPaths {
    pub include_dir: PathBuf,
    pub lib_dir: Option<PathBuf>,
}

/// Ordered, unique installation prefixes; an empty override is ignored.
pub fn search_prefixes(prefix: Option<&Path>) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    for path in prefix
        .filter(|path| !path.as_os_str().is_empty())
        .into_iter()
        .chain([Path::new("/usr/local"), Path::new("/usr")])
    {
        if !paths.iter().any(|entry| entry == path) {
            paths.push(path.to_path_buf());
        }
    }
    paths
}

/// One actionable error for an absent or incomplete development installation.
pub fn not_found_message(prefixes: &[PathBuf]) -> String {
    let searched = prefixes
        .iter()
        .map(|path| path.display().to_string())
        .collect::<Vec<_>>()
        .join(", ");
    format!(
        "vmafx-sys: libvmaf not found (looked in: {searched}); \
         set LIBVMAF_PREFIX or install libvmaf development files"
    )
}

/// Select a complete installation, or tree headers without linking for docs.rs.
/// Filesystem probes are injected so policy tests need no native installation.
pub fn find_paths(
    manifest: &Path,
    prefixes: &[PathBuf],
    docs_rs: bool,
    is_file: impl Fn(&Path) -> bool,
    mut find_library: impl FnMut(&Path) -> Option<PathBuf>,
) -> Result<BuildPaths, String> {
    if docs_rs {
        let include_dir = manifest.join("../../../core/include");
        let header = include_dir.join("libvmaf/libvmaf.h");
        if !is_file(&header) {
            return Err(format!(
                "vmafx-sys: DOCS_RS header not found: {}; use a source checkout with core/include/libvmaf/",
                header.display()
            ));
        }
        return Ok(BuildPaths {
            include_dir,
            lib_dir: None,
        });
    }
    for prefix in prefixes {
        let include_dir = prefix.join("include");
        if is_file(&include_dir.join("libvmaf/libvmaf.h"))
            && let Some(lib_dir) = find_library(prefix)
        {
            return Ok(BuildPaths {
                include_dir,
                lib_dir: Some(lib_dir),
            });
        }
    }
    Err(not_found_message(prefixes))
}
