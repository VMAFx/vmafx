// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// header_check.rs — tests of the build script's stale-header check (#2786).
//
// The build script includes build_support/header_check.rs; so does this test.
// It holds three claims: a planted binding from an older header is refused
// with the header path and the missing names, the bindings of this build pass,
// and the required list names every bound libvmaf name the crates use.

#[path = "../build_support/header_check.rs"]
mod header_check;

use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};

/// Bindings as bindgen writes them for a libvmaf.h from before the MEDIAN and
/// PERC pooling methods (the header of a 2026-04-08 install).
const STALE_BINDINGS: &str = "\
pub const VmafPoolingMethod_VMAF_POOL_METHOD_UNKNOWN: VmafPoolingMethod = 0;
pub const VmafPoolingMethod_VMAF_POOL_METHOD_MIN: VmafPoolingMethod = 1;
pub const VmafPoolingMethod_VMAF_POOL_METHOD_MAX: VmafPoolingMethod = 2;
pub const VmafPoolingMethod_VMAF_POOL_METHOD_MEAN: VmafPoolingMethod = 3;
pub const VmafPoolingMethod_VMAF_POOL_METHOD_HARMONIC_MEAN: VmafPoolingMethod = 4;
pub const VmafPoolingMethod_VMAF_POOL_METHOD_NB: VmafPoolingMethod = 5;
pub type VmafPoolingMethod = ::std::os::raw::c_uint;
";

const BINDINGS: &str = include_str!(concat!(env!("OUT_DIR"), "/bindings.rs"));

/// The bindings of this build with every line that names `name` removed.
fn bindings_without(name: &str) -> String {
    BINDINGS
        .lines()
        .filter(|line| !header_check::identifiers(line).contains(name))
        .collect::<Vec<_>>()
        .join("\n")
}

#[test]
fn stale_header_is_refused_with_path_and_names() {
    let missing = header_check::missing_symbols(STALE_BINDINGS);
    for name in [
        "VmafPoolingMethod_VMAF_POOL_METHOD_MEDIAN",
        "VmafPoolingMethod_VMAF_POOL_METHOD_PERC5",
        "VmafPoolingMethod_VMAF_POOL_METHOD_PERC10",
        "VmafPoolingMethod_VMAF_POOL_METHOD_PERC20",
        "vmaf_score_pooled",
    ] {
        assert!(missing.contains(&name), "{name} not reported: {missing:?}");
    }
    let message = header_check::stale_header_message("/stale/include/libvmaf/libvmaf.h", &missing);
    assert!(
        message.contains("/stale/include/libvmaf/libvmaf.h"),
        "{message}"
    );
    assert!(
        message.contains("VmafPoolingMethod_VMAF_POOL_METHOD_PERC20"),
        "{message}"
    );
}

#[test]
fn bindings_of_this_build_pass() {
    assert_eq!(header_check::missing_symbols(BINDINGS), Vec::<&str>::new());
}

#[test]
fn one_missing_name_is_reported_alone() {
    let missing = header_check::missing_symbols(&bindings_without("vmaf_read_pictures"));
    assert_eq!(missing, vec!["vmaf_read_pictures"]);
}

#[test]
fn a_prefix_of_a_name_does_not_count_as_the_name() {
    // PERC5 must not be found inside PERC50 or a longer identifier.
    let planted = bindings_without("VmafPoolingMethod_VMAF_POOL_METHOD_PERC5")
        + "\npub const VmafPoolingMethod_VMAF_POOL_METHOD_PERC50: u32 = 9;";
    let missing = header_check::missing_symbols(&planted);
    assert_eq!(missing, vec!["VmafPoolingMethod_VMAF_POOL_METHOD_PERC5"]);
}

/// Upper bound on directories walked, so the walk always ends (HISS-02).
const MAX_DIRS: usize = 256;

fn rust_files(root: &Path, out: &mut Vec<PathBuf>) {
    let mut pending = vec![root.to_path_buf()];
    let mut walked = 0;
    while let Some(dir) = pending.pop() {
        walked += 1;
        assert!(
            walked <= MAX_DIRS,
            "more than {MAX_DIRS} directories under {}",
            root.display()
        );
        let mut entries: Vec<_> = fs::read_dir(&dir)
            .unwrap_or_else(|e| panic!("read {}: {e}", dir.display()))
            .map(|entry| entry.map(|e| e.path()))
            .collect::<Result<_, _>>()
            .unwrap_or_else(|e| panic!("list {}: {e}", dir.display()));
        entries.sort();
        for path in entries {
            if path.is_dir() {
                pending.push(path);
            } else if path.extension().is_some_and(|ext| ext == "rs") {
                out.push(path);
            }
        }
    }
}

/// Source without `//` comments, so a name in prose is not taken for a use.
fn code_only(source: &str) -> String {
    source
        .lines()
        .map(|line| line.split("//").next().unwrap_or(""))
        .collect::<Vec<_>>()
        .join("\n")
}

#[test]
fn required_list_covers_every_bound_name_the_crates_use() {
    let manifest = Path::new(env!("CARGO_MANIFEST_DIR"));
    let mut files = vec![manifest.join("src/safe.rs")];
    rust_files(&manifest.join("../vmafx/src"), &mut files);
    assert!(files.len() > 1, "no vmafx sources found");

    let bound = header_check::identifiers(BINDINGS);
    let required: BTreeSet<&str> = header_check::REQUIRED_SYMBOLS.iter().copied().collect();
    let mut unlisted = BTreeSet::new();
    for file in &files {
        let source =
            fs::read_to_string(file).unwrap_or_else(|e| panic!("read {}: {e}", file.display()));
        let code = code_only(&source);
        for name in header_check::identifiers(&code) {
            let libvmaf =
                name.starts_with("Vmaf") || name.starts_with("vmaf_") || name.starts_with("VMAF_");
            if libvmaf && bound.contains(name) && !required.contains(name) {
                unlisted.insert(format!("{name} ({})", file.display()));
            }
        }
    }
    assert!(unlisted.is_empty(), "add to REQUIRED_SYMBOLS: {unlisted:?}");
}
