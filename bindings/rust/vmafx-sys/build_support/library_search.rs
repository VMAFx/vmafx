// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LinkKind {
    Dylib,
    Static,
}

#[derive(Debug, PartialEq, Eq)]
pub struct BuildPaths {
    pub include_dir: PathBuf,
    pub lib_dir: Option<PathBuf>,
    pub link_kind: Option<LinkKind>,
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
        "vmafx-sys: libvmaf and libvmafx not found (looked in: {searched}); \
         set LIBVMAF_PREFIX, install libvmaf development files, or enable the vendored feature"
    )
}

/// Resolve the vendored C core source directory from VMAFX_VENDOR_DIR or repo checkout.
pub fn resolve_vendor_dir(
    vendor_env: Option<&Path>,
    manifest: &Path,
    is_dir: impl Fn(&Path) -> bool,
) -> Result<PathBuf, String> {
    if let Some(path) = vendor_env.filter(|p| !p.as_os_str().is_empty()) {
        if is_dir(path) {
            return Ok(path.to_path_buf());
        }
        return Err(format!(
            "vmafx-sys: VMAFX_VENDOR_DIR does not exist: {}",
            path.display()
        ));
    }
    let core = manifest.join("../../../core");
    if is_dir(&core) {
        return Ok(core);
    }
    Err(
        "vmafx-sys: vendored source directory not found; set VMAFX_VENDOR_DIR or build from a checkout with core/".into(),
    )
}

/// Meson flags disabling all GPU backends and optional targets for a CPU-only vendored build.
pub fn meson_cpu_args() -> &'static [&'static str] {
    &[
        "-Denable_cuda=false",
        "-Denable_nvtx=false",
        "-Denable_nvcc=false",
        "-Denable_sycl=false",
        "-Denable_hip=false",
        "-Denable_hipcc=false",
        "-Denable_metal=disabled",
        "-Denable_dnn=disabled",
        "-Denable_tests=false",
        "-Denable_docs=false",
        "-Denable_tools=false",
        "-Ddefault_library=static",
        "-Db_lto=false",
    ]
}

/// Build the vendored C core with meson and ninja into OUT_DIR.
pub fn build_vendored(
    source_dir: &Path,
    out_dir: &Path,
    reconfigure: bool,
    num_jobs: Option<&str>,
    mut run_cmd: impl FnMut(&str, &[&str]) -> Result<(), String>,
) -> Result<BuildPaths, String> {
    let build_dir = out_dir.join("build");
    let build_str = match build_dir.to_str() {
        Some(s) => s,
        None => return Err("vmafx-sys: invalid UTF-8 in build directory path".into()),
    };
    let source_str = match source_dir.to_str() {
        Some(s) => s,
        None => return Err("vmafx-sys: invalid UTF-8 in source directory path".into()),
    };

    let mut meson_args = vec!["setup"];
    if reconfigure {
        meson_args.push("--reconfigure");
    }
    meson_args.push(build_str);
    meson_args.push(source_str);
    meson_args.extend(meson_cpu_args());

    run_cmd("meson", &meson_args)?;

    let mut ninja_args = vec!["-C", build_str];
    if let Some(jobs) = num_jobs {
        ninja_args.push("-j");
        ninja_args.push(jobs);
    }
    run_cmd("ninja", &ninja_args)?;

    Ok(BuildPaths {
        include_dir: source_dir.join("include"),
        lib_dir: Some(build_dir.join("src")),
        link_kind: Some(LinkKind::Static),
    })
}

fn find_docs_rs_paths(
    manifest: &Path,
    is_file: impl Fn(&Path) -> bool,
) -> Result<BuildPaths, String> {
    let include_dir = manifest.join("../../../core/include");
    let header = include_dir.join("libvmaf/libvmaf.h");
    if !is_file(&header) {
        return Err(format!(
            "vmafx-sys: DOCS_RS header not found: {}; use a source checkout with core/include/libvmaf/",
            header.display()
        ));
    }
    Ok(BuildPaths {
        include_dir,
        lib_dir: None,
        link_kind: None,
    })
}

fn find_system_paths(
    prefixes: &[PathBuf],
    is_file: impl Fn(&Path) -> bool,
    mut find_library: impl FnMut(&Path) -> Option<PathBuf>,
) -> Result<BuildPaths, String> {
    for prefix in prefixes {
        let include_dir = prefix.join("include");
        if is_file(&include_dir.join("libvmaf/libvmaf.h"))
            && let Some(lib_dir) = find_library(prefix)
        {
            return Ok(BuildPaths {
                include_dir,
                lib_dir: Some(lib_dir),
                link_kind: Some(LinkKind::Dylib),
            });
        }
    }
    Err(not_found_message(prefixes))
}

/// Select a complete installation, tree headers for docs.rs, or vendored build.
/// Filesystem probes are injected so policy tests need no native installation.
pub fn find_paths(
    manifest: &Path,
    prefixes: &[PathBuf],
    docs_rs: bool,
    vendored: bool,
    is_file: impl Fn(&Path) -> bool,
    find_library: impl FnMut(&Path) -> Option<PathBuf>,
    build_vendored: impl FnOnce() -> Result<BuildPaths, String>,
) -> Result<BuildPaths, String> {
    if docs_rs {
        return find_docs_rs_paths(manifest, is_file);
    }
    if vendored {
        return build_vendored();
    }
    find_system_paths(prefixes, is_file, find_library)
}

/// Extract clean library name from archive file name (e.g. libvmaf.a -> vmaf).
pub fn library_name_from_filename(filename: &str) -> Option<String> {
    let s = filename
        .strip_suffix(".a")
        .or_else(|| filename.strip_suffix(".lib"))?;
    let name = s.strip_prefix("lib").unwrap_or(s);
    if name.is_empty() {
        None
    } else {
        Some(name.to_string())
    }
}

/// Place vmaf first and vmafx second for static link resolution; sort the rest.
pub fn order_static_libraries(mut names: Vec<String>) -> Vec<String> {
    let mut ordered = Vec::new();
    if names.iter().any(|n| n == "vmaf") {
        ordered.push("vmaf".to_string());
    }
    if names.iter().any(|n| n == "vmafx") {
        ordered.push("vmafx".to_string());
    }
    names.sort();
    for name in names {
        if name != "vmaf" && name != "vmafx" && !ordered.contains(&name) {
            ordered.push(name);
        }
    }
    ordered
}

/// Discover every static library Meson actually produced under build/src.
pub fn discover_static_libraries(lib_dir: &Path) -> Result<Vec<String>, String> {
    let entries = std::fs::read_dir(lib_dir).map_err(|e| {
        format!(
            "vmafx-sys: failed to read lib directory '{}': {e}",
            lib_dir.display()
        )
    })?;
    let mut names = Vec::new();
    for entry in entries.filter_map(Result::ok).take(256) {
        if let Some(file_name) = entry.file_name().to_str()
            && let Some(name) = library_name_from_filename(file_name)
        {
            names.push(name);
        }
    }
    Ok(order_static_libraries(names))
}

fn parse_so_lib_name(filename: &str) -> Option<String> {
    let without_lib = filename.strip_prefix("lib")?;
    let so_idx = without_lib.find(".so")?;
    let name = &without_lib[..so_idx];
    if name.is_empty() {
        None
    } else {
        Some(name.to_string())
    }
}

fn parse_absolute_lib_token(token: &str, dirs: &mut Vec<PathBuf>, deps: &mut Vec<String>) {
    let path = Path::new(token);
    if let Some(parent) = path.parent() {
        let parent_buf = parent.to_path_buf();
        if !dirs.contains(&parent_buf) {
            dirs.push(parent_buf);
        }
    }
    if let Some(file_name) = path.file_name().and_then(|f| f.to_str())
        && let Some(name) = parse_so_lib_name(file_name)
        && !deps.contains(&name)
    {
        deps.push(name);
    }
}

fn parse_pc_line_tokens(line: &str, dirs: &mut Vec<PathBuf>, deps: &mut Vec<String>) {
    for token in line.split_whitespace().take(128) {
        if token == "-pthread" {
            if !deps.iter().any(|d| d == "pthread") {
                deps.push("pthread".to_string());
            }
        } else if let Some(name) = token.strip_prefix("-l") {
            if name != "vmaf"
                && name != "vmafx"
                && !name.is_empty()
                && !deps.iter().any(|d| d == name)
            {
                deps.push(name.to_string());
            }
        } else if let Some(dir) = token.strip_prefix("-L") {
            if !dir.starts_with("${") && !dir.is_empty() {
                let p = PathBuf::from(dir);
                if !dirs.contains(&p) {
                    dirs.push(p);
                }
            }
        } else if token.starts_with('/') {
            parse_absolute_lib_token(token, dirs, deps);
        }
    }
}

/// Parse search directories and dependency library names from pkg-config file content.
pub fn parse_pc_dependencies(content: &str) -> (Vec<PathBuf>, Vec<String>) {
    let mut dirs = Vec::new();
    let mut deps = Vec::new();
    for line in content.lines().take(512) {
        let trimmed = line.trim();
        if let Some(rest) = trimmed
            .strip_prefix("Libs:")
            .or_else(|| trimmed.strip_prefix("Libs.private:"))
        {
            parse_pc_line_tokens(rest, &mut dirs, &mut deps);
        }
    }
    (dirs, deps)
}

fn merge_unique<T: PartialEq>(into: &mut Vec<T>, from: Vec<T>) {
    for item in from {
        if !into.contains(&item) {
            into.push(item);
        }
    }
}

/// Module names on the `Requires` lines of pkg-config file content, without version constraints.
pub fn parse_pc_requires(content: &str) -> Vec<String> {
    let mut modules = Vec::new();
    for line in content.lines().take(512) {
        let trimmed = line.trim();
        let Some(rest) = trimmed
            .strip_prefix("Requires:")
            .or_else(|| trimmed.strip_prefix("Requires.private:"))
        else {
            continue;
        };
        let mut version_follows = false;
        for token in rest.replace(',', " ").split_whitespace().take(128) {
            if version_follows {
                version_follows = false;
            } else if matches!(token, "=" | "!=" | "<" | "<=" | ">" | ">=") {
                version_follows = true;
            } else if !modules.iter().any(|module| module == token) {
                modules.push(token.to_string());
            }
        }
    }
    modules
}

/// Read all .pc files under build_dir/meson-private and aggregate static dependencies.
///
/// A required module without a file there (zimg) is resolved through `query`, which
/// returns its link flags; a failed query fails the build instead of a later link.
pub fn read_pc_dependencies(
    build_dir: &Path,
    mut query: impl FnMut(&str) -> Result<String, String>,
) -> Result<(Vec<PathBuf>, Vec<String>), String> {
    let mut all_dirs = Vec::new();
    let mut all_deps = Vec::new();
    let private_dir = build_dir.join("meson-private");
    let Ok(entries) = std::fs::read_dir(&private_dir) else {
        return Ok((all_dirs, all_deps));
    };
    for entry in entries.filter_map(Result::ok).take(64) {
        let path = entry.path();
        if path.extension().and_then(|ext| ext.to_str()) != Some("pc") {
            continue;
        }
        let Ok(content) = std::fs::read_to_string(&path) else {
            continue;
        };
        let (dirs, deps) = parse_pc_dependencies(&content);
        merge_unique(&mut all_dirs, dirs);
        merge_unique(&mut all_deps, deps);
        for module in parse_pc_requires(&content) {
            if !private_dir.join(format!("{module}.pc")).is_file() {
                parse_pc_line_tokens(&query(&module)?, &mut all_dirs, &mut all_deps);
            }
        }
    }
    Ok((all_dirs, all_deps))
}

/// Extract VMAF_VERSION from generated vcs_version.h header.
pub fn parse_vcs_version_header(content: &str) -> Option<String> {
    for line in content.lines().take(64) {
        let trimmed = line.trim();
        if let Some(rest) = trimmed.strip_prefix("#define VMAF_VERSION") {
            let val = rest.trim().trim_matches('"');
            if !val.is_empty() {
                return Some(val.to_string());
            }
        }
    }
    None
}

/// Extract project version from core/meson.build.
pub fn parse_meson_project_version(content: &str) -> Option<String> {
    for line in content.lines().take(128) {
        let trimmed = line.trim();
        if (trimmed.starts_with("version :") || trimmed.starts_with("version:"))
            && let Some((_, after)) = trimmed.split_once(':')
        {
            let trimmed_after = after.trim();
            if let Some(quote) = trimmed_after.chars().next()
                && (quote == '\'' || quote == '"')
                && let Some(rest) = trimmed_after.strip_prefix(quote)
                && let Some((inside, _)) = rest.split_once(quote)
            {
                return Some(inside.to_string());
            }
        }
    }
    None
}

/// Extract vendored core version from vcs_version.h or meson.build.
pub fn extract_core_version(source_dir: &Path, out_dir: &Path) -> Result<String, String> {
    let vcs_header = out_dir.join("build/include/vcs_version.h");
    if let Ok(content) = std::fs::read_to_string(&vcs_header)
        && let Some(v) = parse_vcs_version_header(&content)
    {
        return Ok(v);
    }
    let meson_build = source_dir.join("meson.build");
    if let Ok(content) = std::fs::read_to_string(&meson_build)
        && let Some(v) = parse_meson_project_version(&content)
    {
        return Ok(v);
    }
    Err("vmafx-sys: could not determine vendored core version".into())
}
