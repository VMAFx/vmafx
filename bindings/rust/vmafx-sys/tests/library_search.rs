// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

#[path = "../build_support/library_search.rs"]
mod library_search;

use library_search::{
    BuildPaths, LinkKind, build_vendored, discover_static_libraries, extract_core_version,
    find_paths, library_name_from_filename, not_found_message, order_static_libraries,
    parse_meson_project_version, parse_pc_dependencies, parse_pc_requires,
    parse_vcs_version_header, read_pc_dependencies, resolve_vendor_dir, search_prefixes,
};
use std::cell::RefCell;
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
        false,
        |path| path == Path::new("/custom/include/libvmaf/libvmaf.h"),
        |prefix| Some(prefix.join("lib")),
        || Err("unreachable in test".into()),
    )?;
    assert_eq!(paths.include_dir, Path::new("/custom/include"));
    assert_eq!(paths.lib_dir, Some(PathBuf::from("/custom/lib")));
    assert_eq!(paths.link_kind, Some(LinkKind::Dylib));
    Ok(())
}

#[test]
fn missing_install_has_named_error_and_ordered_paths() {
    let result = find_paths(
        Path::new("/repo/bindings/rust/vmafx-sys"),
        &search_prefixes(Some(Path::new("/missing"))),
        false,
        false,
        |_| false,
        |_| None,
        || Err("unreachable in test".into()),
    );
    assert_eq!(
        result,
        Err("vmafx-sys: libvmaf and libvmafx not found (looked in: /missing, /usr/local, /usr); set LIBVMAF_PREFIX, install libvmaf development files, or enable the vendored feature".into())
    );
}

#[test]
fn missing_install_error_contains_all_required_tokens() {
    let prefixes = search_prefixes(Some(Path::new("/missing")));
    let err = not_found_message(&prefixes);
    assert!(err.contains("vmafx-sys"));
    assert!(err.contains("libvmaf"));
    assert!(err.contains("libvmafx"));
    assert!(err.contains("LIBVMAF_PREFIX"));
    assert!(err.contains("vendored"));
    assert!(err.contains("/missing, /usr/local, /usr"));
}

#[test]
fn header_without_library_is_not_an_install() {
    let result = find_paths(
        Path::new("/repo/bindings/rust/vmafx-sys"),
        &search_prefixes(None),
        false,
        false,
        |_| true,
        |_| None,
        || Err("unreachable in test".into()),
    );
    assert!(result.is_err());
}

#[test]
fn incomplete_prefix_falls_back_to_complete_system_install() -> Result<(), String> {
    let paths = find_paths(
        Path::new("/repo/bindings/rust/vmafx-sys"),
        &search_prefixes(Some(Path::new("/incomplete"))),
        false,
        false,
        |_| true,
        |prefix| (prefix == Path::new("/usr")).then(|| prefix.join("lib")),
        || Err("unreachable in test".into()),
    )?;
    assert_eq!(paths.include_dir, Path::new("/usr/include"));
    assert_eq!(paths.lib_dir, Some(PathBuf::from("/usr/lib")));
    assert_eq!(paths.link_kind, Some(LinkKind::Dylib));
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
        false,
        |path| path == include.join("libvmaf/libvmaf.h"),
        |_| {
            probes += 1;
            None
        },
        || Err("unreachable in test".into()),
    )?;
    assert_eq!(paths.include_dir, include);
    assert_eq!(paths.lib_dir, None);
    assert_eq!(paths.link_kind, None);
    assert_eq!(probes, 0);
    Ok(())
}

#[test]
fn docs_rs_refuses_missing_tree_header_without_system_fallback() {
    let manifest = Path::new("/repo/bindings/rust/vmafx-sys");
    let result = find_paths(
        manifest,
        &search_prefixes(None),
        true,
        false,
        |_| false,
        |_| None,
        || Err("unreachable in test".into()),
    );
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

#[test]
fn docs_rs_bypasses_vendored_build() -> Result<(), String> {
    let manifest = Path::new("/repo/bindings/rust/vmafx-sys");
    let include = manifest.join("../../../core/include");
    let paths = find_paths(
        manifest,
        &search_prefixes(None),
        true,
        true,
        |path| path == include.join("libvmaf/libvmaf.h"),
        |_| None,
        || Err("build_vendored should not be called when docs_rs is true".into()),
    )?;
    assert_eq!(paths.include_dir, include);
    assert_eq!(paths.lib_dir, None);
    assert_eq!(paths.link_kind, None);
    Ok(())
}

#[test]
fn resolve_vendor_dir_prefers_valid_env_override() -> Result<(), String> {
    let custom = Path::new("/custom/vmafx-source");
    let manifest = Path::new("/repo/bindings/rust/vmafx-sys");
    let dir = resolve_vendor_dir(Some(custom), manifest, |p| p == custom)?;
    assert_eq!(dir, custom);
    Ok(())
}

#[test]
fn resolve_vendor_dir_rejects_missing_env_override() {
    let custom = Path::new("/custom/nonexistent");
    let manifest = Path::new("/repo/bindings/rust/vmafx-sys");
    let result = resolve_vendor_dir(Some(custom), manifest, |_| false);
    assert_eq!(
        result,
        Err("vmafx-sys: VMAFX_VENDOR_DIR does not exist: /custom/nonexistent".into())
    );
}

#[test]
fn resolve_vendor_dir_falls_back_to_repo_core() -> Result<(), String> {
    let manifest = Path::new("/repo/bindings/rust/vmafx-sys");
    let expected_core = manifest.join("../../../core");
    let dir = resolve_vendor_dir(None, manifest, |p| p == expected_core)?;
    assert_eq!(dir, expected_core);
    Ok(())
}

#[test]
fn resolve_vendor_dir_rejects_missing_repo_core() {
    let manifest = Path::new("/repo/bindings/rust/vmafx-sys");
    let result = resolve_vendor_dir(None, manifest, |_| false);
    assert_eq!(
        result,
        Err("vmafx-sys: vendored source directory not found; set VMAFX_VENDOR_DIR or build from a checkout with core/".into())
    );
}

#[test]
fn build_vendored_runs_meson_and_ninja_cpu_only() -> Result<(), String> {
    let source_dir = Path::new("/repo/core");
    let out_dir = Path::new("/target/out");
    let calls: RefCell<Vec<(String, Vec<String>)>> = RefCell::new(Vec::new());

    let paths = build_vendored(source_dir, out_dir, false, Some("4"), |program, args| {
        calls.borrow_mut().push((
            program.to_string(),
            args.iter().map(|s| (*s).to_string()).collect(),
        ));
        Ok(())
    })?;

    assert_eq!(paths.include_dir, source_dir.join("include"));
    assert_eq!(paths.lib_dir, Some(out_dir.join("build/src")));
    assert_eq!(paths.link_kind, Some(LinkKind::Static));

    let recorded = calls.into_inner();
    assert_eq!(recorded.len(), 2);
    assert_eq!(recorded[0].0, "meson");
    assert!(recorded[0].1.contains(&"setup".to_string()));
    assert!(recorded[0].1.contains(&"-Denable_cuda=false".to_string()));
    assert!(recorded[0].1.contains(&"-Denable_sycl=false".to_string()));
    assert!(recorded[0].1.contains(&"-Denable_hip=false".to_string()));
    assert!(
        recorded[0]
            .1
            .contains(&"-Denable_metal=disabled".to_string())
    );
    assert!(recorded[0].1.contains(&"-Denable_dnn=disabled".to_string()));
    assert!(
        recorded[0]
            .1
            .contains(&"-Ddefault_library=static".to_string())
    );
    assert!(recorded[0].1.contains(&"-Db_lto=false".to_string()));
    assert_eq!(recorded[1].0, "ninja");
    assert_eq!(
        recorded[1].1,
        vec![
            "-C".to_string(),
            out_dir.join("build").to_str().map_or("", |s| s).to_string(),
            "-j".to_string(),
            "4".to_string()
        ]
    );
    Ok(())
}

#[test]
fn build_vendored_omits_job_flag_when_num_jobs_absent() -> Result<(), String> {
    let source_dir = Path::new("/repo/core");
    let out_dir = Path::new("/target/out");
    let calls: RefCell<Vec<(String, Vec<String>)>> = RefCell::new(Vec::new());

    let paths = build_vendored(source_dir, out_dir, false, None, |program, args| {
        calls.borrow_mut().push((
            program.to_string(),
            args.iter().map(|s| (*s).to_string()).collect(),
        ));
        Ok(())
    })?;

    assert_eq!(paths.link_kind, Some(LinkKind::Static));
    let recorded = calls.into_inner();
    assert_eq!(recorded.len(), 2);
    assert_eq!(recorded[1].0, "ninja");
    assert_eq!(
        recorded[1].1,
        vec![
            "-C".to_string(),
            out_dir.join("build").to_str().map_or("", |s| s).to_string(),
        ]
    );
    Ok(())
}

#[test]
fn build_vendored_passes_reconfigure_flag() -> Result<(), String> {
    let source_dir = Path::new("/repo/core");
    let out_dir = Path::new("/target/out");
    let mut reconfigure_seen = false;

    let _ = build_vendored(source_dir, out_dir, true, None, |program, args| {
        if program == "meson" && args.contains(&"--reconfigure") {
            reconfigure_seen = true;
        }
        Ok(())
    })?;

    assert!(reconfigure_seen);
    Ok(())
}

#[test]
fn build_vendored_returns_error_on_meson_failure() {
    let source_dir = Path::new("/repo/core");
    let out_dir = Path::new("/target/out");
    let result = build_vendored(source_dir, out_dir, false, None, |program, _| {
        if program == "meson" {
            Err("vmafx-sys: meson failed with exit status 1: command was 'meson setup'".into())
        } else {
            Ok(())
        }
    });
    assert_eq!(
        result,
        Err("vmafx-sys: meson failed with exit status 1: command was 'meson setup'".into())
    );
}

#[test]
fn build_vendored_returns_error_on_ninja_failure() {
    let source_dir = Path::new("/repo/core");
    let out_dir = Path::new("/target/out");
    let result = build_vendored(source_dir, out_dir, false, None, |program, _| {
        if program == "ninja" {
            Err("vmafx-sys: ninja failed with exit status 1: command was 'ninja -C build'".into())
        } else {
            Ok(())
        }
    });
    assert_eq!(
        result,
        Err("vmafx-sys: ninja failed with exit status 1: command was 'ninja -C build'".into())
    );
}

#[test]
fn vendored_flag_invokes_builder_without_system_search() -> Result<(), String> {
    let manifest = Path::new("/repo/bindings/rust/vmafx-sys");
    let mut builder_called = false;
    let paths = find_paths(
        manifest,
        &search_prefixes(None),
        false,
        true,
        |_| false,
        |_| panic!("find_library must not be called when vendored is true"),
        || {
            builder_called = true;
            Ok(BuildPaths {
                include_dir: PathBuf::from("/vendored/include"),
                lib_dir: Some(PathBuf::from("/vendored/lib")),
                link_kind: Some(LinkKind::Static),
            })
        },
    )?;
    assert!(builder_called);
    assert_eq!(paths.include_dir, Path::new("/vendored/include"));
    assert_eq!(paths.lib_dir, Some(PathBuf::from("/vendored/lib")));
    assert_eq!(paths.link_kind, Some(LinkKind::Static));
    Ok(())
}

#[test]
fn vendored_builder_error_does_not_fall_back_to_system() {
    let manifest = Path::new("/repo/bindings/rust/vmafx-sys");
    let result = find_paths(
        manifest,
        &search_prefixes(None),
        false,
        true,
        |_| true,
        |prefix| Some(prefix.join("lib")),
        || Err("vmafx-sys: vendored compilation failed".into()),
    );
    assert_eq!(result, Err("vmafx-sys: vendored compilation failed".into()));
}

#[test]
fn library_name_extraction() {
    assert_eq!(
        library_name_from_filename("libvmaf.a"),
        Some("vmaf".to_string())
    );
    assert_eq!(
        library_name_from_filename("libvmafx.a"),
        Some("vmafx".to_string())
    );
    assert_eq!(
        library_name_from_filename("liblibsvm.a"),
        Some("libsvm".to_string())
    );
    assert_eq!(
        library_name_from_filename("vmaf.lib"),
        Some("vmaf".to_string())
    );
    assert_eq!(library_name_from_filename("not_a_lib.txt"), None);
}

#[test]
fn static_libraries_ordered_vmaf_then_vmafx_then_rest() {
    let input = vec![
        "x86_avx2".to_string(),
        "vmafx".to_string(),
        "libsvm".to_string(),
        "vmaf".to_string(),
    ];
    let ordered = order_static_libraries(input);
    assert_eq!(ordered, vec!["vmaf", "vmafx", "libsvm", "x86_avx2"]);
}

#[test]
fn pc_dependencies_parsed_correctly() {
    let sample = r#"
prefix=/usr/local
libdir=${prefix}/lib
Libs: -L${libdir} -lvmafx -pthread -lm /usr/lib64/libonnxruntime.so.1.20.1
Libs.private: -lstdc++
"#;
    let (dirs, deps) = parse_pc_dependencies(sample);
    assert_eq!(dirs, vec![PathBuf::from("/usr/lib64")]);
    assert_eq!(deps, vec!["pthread", "m", "onnxruntime", "stdc++"]);
}

#[test]
fn version_header_and_meson_parser() {
    let header = "/* generated */\n#define VMAF_VERSION \"v1.0.0-rc.3-604-gbdd7f9c70\"\n";
    assert_eq!(
        parse_vcs_version_header(header),
        Some("v1.0.0-rc.3-604-gbdd7f9c70".to_string())
    );

    let meson = "project('libvmaf', ['c', 'cpp'],\n    version : '1.0.0-rc.3',\n";
    assert_eq!(
        parse_meson_project_version(meson),
        Some("1.0.0-rc.3".to_string())
    );
}

#[test]
fn discover_static_libraries_in_dir() -> Result<(), String> {
    let temp = std::env::temp_dir().join("vmafx_test_discover_libs");
    let _ = std::fs::remove_dir_all(&temp);
    std::fs::create_dir_all(&temp).map_err(|e| e.to_string())?;
    std::fs::write(temp.join("libx86.a"), b"").map_err(|e| e.to_string())?;
    std::fs::write(temp.join("libvmafx.a"), b"").map_err(|e| e.to_string())?;
    std::fs::write(temp.join("libvmaf.a"), b"").map_err(|e| e.to_string())?;

    let discovered = discover_static_libraries(&temp)?;
    let _ = std::fs::remove_dir_all(&temp);
    assert_eq!(discovered, vec!["vmaf", "vmafx", "x86"]);
    Ok(())
}

#[test]
fn read_pc_dependencies_aggregates_files() -> Result<(), String> {
    let temp = std::env::temp_dir().join("vmafx_test_read_pc");
    let _ = std::fs::remove_dir_all(&temp);
    let meson_priv = temp.join("meson-private");
    std::fs::create_dir_all(&meson_priv).map_err(|e| e.to_string())?;
    let content = "Libs: -pthread -lm\nLibs.private: -lstdc++\n";
    std::fs::write(meson_priv.join("libvmafx.pc"), content).map_err(|e| e.to_string())?;

    let result = read_pc_dependencies(&temp, |module| Err(format!("unexpected query: {module}")));
    let _ = std::fs::remove_dir_all(&temp);
    assert_eq!(result?.1, vec!["pthread", "m", "stdc++"]);
    Ok(())
}

#[test]
fn pc_requires_drop_version_constraints() {
    let sample = "Requires: zimg >= 2.7, libfoo\nRequires.private: libbar = 1.0 libfoo\nName: x\n";
    assert_eq!(parse_pc_requires(sample), vec!["zimg", "libfoo", "libbar"]);
    assert!(parse_pc_requires("Name: x\nLibs: -lm\n").is_empty());
}

fn pc_tree(name: &str, content: &str) -> Result<PathBuf, String> {
    let temp = std::env::temp_dir().join(name);
    let _ = std::fs::remove_dir_all(&temp);
    let meson_priv = temp.join("meson-private");
    std::fs::create_dir_all(&meson_priv).map_err(|e| e.to_string())?;
    std::fs::write(meson_priv.join("libvmafx.pc"), content).map_err(|e| e.to_string())?;
    Ok(temp)
}

#[test]
fn read_pc_dependencies_resolves_required_modules() -> Result<(), String> {
    let temp = pc_tree(
        "vmafx_test_pc_requires",
        "Requires: zimg >= 2.7\nLibs: -lm\n",
    )?;
    let result = read_pc_dependencies(&temp, |module| {
        assert_eq!(module, "zimg");
        Ok("-L/opt/zimg/lib -lzimg -lm\n".to_string())
    });
    let _ = std::fs::remove_dir_all(&temp);
    let (dirs, deps) = result?;
    assert_eq!(dirs, vec![PathBuf::from("/opt/zimg/lib")]);
    assert_eq!(deps, vec!["m", "zimg"]);
    Ok(())
}

#[test]
fn read_pc_dependencies_fails_on_unresolved_module() -> Result<(), String> {
    let temp = pc_tree("vmafx_test_pc_unresolved", "Requires: zimg\n")?;
    let result = read_pc_dependencies(&temp, |_| Err("no zimg".to_string()));
    let _ = std::fs::remove_dir_all(&temp);
    assert_eq!(result, Err("no zimg".to_string()));
    Ok(())
}

#[test]
fn read_pc_dependencies_skips_sibling_pc_files() -> Result<(), String> {
    let temp = pc_tree("vmafx_test_pc_sibling", "Libs: -lm\n")?;
    let sibling = temp.join("meson-private/libvmaf.pc");
    std::fs::write(sibling, "Requires: libvmafx\n").map_err(|e| e.to_string())?;
    let result = read_pc_dependencies(&temp, |module| Err(format!("queried {module}")));
    let _ = std::fs::remove_dir_all(&temp);
    assert_eq!(result?.1, vec!["m"]);
    Ok(())
}

#[test]
fn extract_core_version_finds_vcs_or_meson() -> Result<(), String> {
    let temp = std::env::temp_dir().join("vmafx_test_extract_ver");
    let _ = std::fs::remove_dir_all(&temp);
    let out = temp.join("out/build/include");
    let src = temp.join("src");
    std::fs::create_dir_all(&out).map_err(|e| e.to_string())?;
    std::fs::create_dir_all(&src).map_err(|e| e.to_string())?;
    std::fs::write(
        out.join("vcs_version.h"),
        "#define VMAF_VERSION \"v1.0.0-rc.3-test\"\n",
    )
    .map_err(|e| e.to_string())?;

    let version = extract_core_version(&src, &temp.join("out"))?;
    let _ = std::fs::remove_dir_all(&temp);
    assert_eq!(version, "v1.0.0-rc.3-test");
    Ok(())
}
