// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// tests/netflix_bitexact.rs — bit-exact parity with C API vmafx_score_contract (#2434).

use std::env;
use std::io::Read;
use std::path::{Path, PathBuf};

const WIDTH: u32 = 576;
const HEIGHT: u32 = 324;
const MAX_FRAMES: u32 = 4096;

unsafe extern "C" {
    fn strtod(nptr: *const std::ffi::c_char, endptr: *mut *mut std::ffi::c_char) -> f64;
}

struct InputPaths {
    reference: PathBuf,
    distorted: PathBuf,
    model: PathBuf,
    contract_bin: PathBuf,
}

struct CScorePayload {
    vmaf: f64,
    vmaf_hex: String,
    frames: u32,
}

fn repo_root() -> PathBuf {
    if let Ok(v) = env::var("VMAFX_REPO") {
        return PathBuf::from(v);
    }
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    manifest_dir
        .ancestors()
        .find(|dir| {
            (dir.join("meson.build").exists() || dir.join("Cargo.toml").exists())
                && dir.join("model").exists()
        })
        .map_or_else(|| PathBuf::from("."), Path::to_path_buf)
}

fn resolve_yuv_path(root: &Path, env_direct: &str, alt_env: &str, default_file: &str) -> PathBuf {
    if let Ok(v) = env::var(env_direct) {
        PathBuf::from(v)
    } else if let Ok(v) = env::var(alt_env) {
        PathBuf::from(v)
    } else if let Ok(dir) = env::var("VMAFX_YUV_DIR") {
        PathBuf::from(dir).join(default_file)
    } else if let Ok(dir) = env::var("VMAFX_CONTRACT_YUV") {
        PathBuf::from(dir).join(default_file)
    } else {
        root.join("python/test/resource/yuv").join(default_file)
    }
}

fn resolve_contract_bin(root: &Path) -> PathBuf {
    if let Ok(v) = env::var("VMAFX_CONTRACT_CAPI") {
        PathBuf::from(v)
    } else if let Ok(v) = env::var("VMAFX_LIBRARY") {
        let lib = PathBuf::from(v);
        lib.parent()
            .and_then(Path::parent)
            .map_or_else(|| root.join(".b/test/vmafx_score_contract"), |p| p.join("test/vmafx_score_contract"))
    } else if let Ok(v) = env::var("LIBVMAF_PREFIX") {
        let p = PathBuf::from(v);
        if p.join("test/vmafx_score_contract").is_file() {
            p.join("test/vmafx_score_contract")
        } else {
            root.join(".b/test/vmafx_score_contract")
        }
    } else if root.join(".b/test/vmafx_score_contract").is_file() {
        root.join(".b/test/vmafx_score_contract")
    } else {
        root.join("build/test/vmafx_score_contract")
    }
}

fn find_inputs() -> InputPaths {
    let root = repo_root();
    let reference = resolve_yuv_path(
        &root,
        "VMAFX_YUV_REF",
        "VMAFX_YUV_REF",
        "src01_hrc00_576x324.yuv",
    );
    let distorted = resolve_yuv_path(
        &root,
        "VMAFX_YUV_DIST",
        "VMAFX_YUV_DIS",
        "src01_hrc01_576x324.yuv",
    );
    let model = if let Ok(v) = env::var("VMAFX_MODEL") {
        PathBuf::from(v)
    } else if let Ok(v) = env::var("VMAFX_CONTRACT_MODEL_DIR") {
        PathBuf::from(v).join("vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json")
    } else {
        root.join("model/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json")
    };
    let contract_bin = resolve_contract_bin(&root);
    InputPaths {
        reference,
        distorted,
        model,
        contract_bin,
    }
}

fn check_inputs(paths: &InputPaths, require_pair: bool) -> Result<bool, Box<dyn std::error::Error>> {
    let mut missing = Vec::new();
    if !paths.reference.is_file() {
        missing.push(format!("ref YUV ({})", paths.reference.display()));
    }
    if !paths.distorted.is_file() {
        missing.push(format!("dist YUV ({})", paths.distorted.display()));
    }
    if !paths.model.is_file() {
        missing.push(format!("model ({})", paths.model.display()));
    }
    if !paths.contract_bin.is_file() {
        missing.push(format!("contract binary ({})", paths.contract_bin.display()));
    }
    if missing.is_empty() {
        return Ok(true);
    }
    let env_required = env::var("VMAFX_REQUIRE_NETFLIX_PAIR").unwrap_or_default();
    let is_required = require_pair || (!env_required.is_empty() && env_required != "0");
    if is_required {
        return Err(format!(
            "VMAFX_REQUIRE_NETFLIX_PAIR is set and inputs are missing: {}",
            missing.join(", ")
        )
        .into());
    }
    println!(
        "SKIP: missing inputs ({}); set VMAFX_YUV_DIR/VMAFX_CONTRACT_CAPI to run",
        missing.join(", ")
    );
    Ok(false)
}

fn parse_hex_float(s: &str) -> Result<f64, Box<dyn std::error::Error>> {
    let c_str = std::ffi::CString::new(s)?;
    // SAFETY: c_str is a valid nul-terminated C string; endptr is null.
    let val = unsafe { strtod(c_str.as_ptr(), std::ptr::null_mut()) };
    Ok(val)
}

fn extract_json_str(json: &str, key: &str) -> Result<String, Box<dyn std::error::Error>> {
    let pos = json.find(key).ok_or("key not found in contract JSON")?;
    let after = &json[pos.saturating_add(key.len())..];
    let first_quote = after.find('"').ok_or("opening quote not found")?;
    let rest = &after[first_quote.saturating_add(1)..];
    let end_quote = rest.find('"').ok_or("closing quote not found")?;
    Ok(rest[..end_quote].to_string())
}

fn extract_json_num(json: &str, key: &str) -> Result<f64, Box<dyn std::error::Error>> {
    let pos = json.find(key).ok_or("key not found in contract JSON")?;
    let after = json[pos.saturating_add(key.len())..].trim_start();
    let end = after
        .find(|c: char| c == ',' || c == '}' || c.is_whitespace())
        .unwrap_or(after.len());
    let val = after[..end].parse::<f64>()?;
    Ok(val)
}

fn parse_c_payload(json: &str) -> Result<CScorePayload, Box<dyn std::error::Error>> {
    let vmaf = extract_json_num(json, "\"vmaf\":")?;
    let vmaf_hex = extract_json_str(json, "\"vmaf_hex\":")?;
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let frames = extract_json_num(json, "\"frames\":")? as u32;

    let hex_f64 = parse_hex_float(&vmaf_hex)?;
    if hex_f64.to_bits() != vmaf.to_bits() {
        return Err(format!(
            "C contract internal mismatch: hex {vmaf_hex} != decimal {vmaf}"
        )
        .into());
    }
    Ok(CScorePayload {
        vmaf,
        vmaf_hex,
        frames,
    })
}

fn run_c_contract(paths: &InputPaths) -> Result<CScorePayload, Box<dyn std::error::Error>> {
    let ref_str = paths.reference.to_str().ok_or("invalid ref path")?;
    let dist_str = paths.distorted.to_str().ok_or("invalid dist path")?;
    let mut cmd = std::process::Command::new(&paths.contract_bin);
    cmd.args([
        ref_str,
        dist_str,
        "576",
        "324",
        "8",
        "vmaf_v1.0.16_3d0h",
        "0",
        "1",
    ]);
    if let Ok(ld) = env::var("LD_LIBRARY_PATH") {
        cmd.env("LD_LIBRARY_PATH", ld);
    }
    let output = cmd.output()?;
    if !output.status.success() {
        return Err(format!(
            "vmafx_score_contract failed (status={}): {}",
            output.status,
            String::from_utf8_lossy(&output.stderr)
        )
        .into());
    }
    let stdout = String::from_utf8(output.stdout)?;
    parse_c_payload(&stdout)
}

#[allow(clippy::similar_names)]
fn read_yuv_frame(
    file: &mut std::fs::File,
    pic: &mut vmafx_sys::VmafPicture,
) -> std::io::Result<bool> {
    let w = WIDTH as usize;
    let h = HEIGHT as usize;
    let luma_bytes = w.saturating_mul(h);
    let mut luma = vec![0u8; luma_bytes];
    let n = file.read(&mut luma)?;
    if n == 0 || n < luma_bytes {
        return Ok(false);
    }
    let chroma_w = w / 2;
    let chroma_h = h / 2;
    let chroma_bytes = chroma_w.saturating_mul(chroma_h);
    let mut cb = vec![0u8; chroma_bytes];
    let mut cr = vec![0u8; chroma_bytes];
    file.read_exact(&mut cb)?;
    file.read_exact(&mut cr)?;

    // SAFETY: allocated picture planes cover the declared dimensions and strides.
    unsafe {
        let luma_plane = pic.data[0].cast::<u8>();
        let cb_plane = pic.data[1].cast::<u8>();
        let cr_plane = pic.data[2].cast::<u8>();

        #[allow(clippy::cast_sign_loss)]
        let luma_stride = pic.stride[0] as usize;
        #[allow(clippy::cast_sign_loss)]
        let cb_stride = pic.stride[1] as usize;
        #[allow(clippy::cast_sign_loss)]
        let cr_stride = pic.stride[2] as usize;

        for row in 0..h {
            let src = &luma[(row * w)..((row + 1) * w)];
            let dst = luma_plane.add(row * luma_stride);
            std::ptr::copy_nonoverlapping(src.as_ptr(), dst, w);
        }
        for row in 0..chroma_h {
            let src_cb = &cb[(row * chroma_w)..((row + 1) * chroma_w)];
            let src_cr = &cr[(row * chroma_w)..((row + 1) * chroma_w)];
            let dst_cb = cb_plane.add(row * cb_stride);
            let dst_cr = cr_plane.add(row * cr_stride);
            std::ptr::copy_nonoverlapping(src_cb.as_ptr(), dst_cb, chroma_w);
            std::ptr::copy_nonoverlapping(src_cr.as_ptr(), dst_cr, chroma_w);
        }
    }
    Ok(true)
}

fn score_pair_rust(paths: &InputPaths) -> Result<(f64, u32), Box<dyn std::error::Error>> {
    let model = vmafx::Model::from_path(&paths.model)?;
    let mut ctx = vmafx::ContextBuilder::new()
        .n_threads(0)
        .n_subsample(1)
        .build()?;
    ctx.use_features_from_model(&model)?;

    let mut ref_file = std::fs::File::open(&paths.reference)?;
    let mut dist_file = std::fs::File::open(&paths.distorted)?;

    let mut n_frames = 0u32;
    for idx in 0..MAX_FRAMES {
        let mut r_pic = vmafx::Picture::new_yuv420p_8bit(WIDTH, HEIGHT)?;
        let mut d_pic = vmafx::Picture::new_yuv420p_8bit(WIDTH, HEIGHT)?;

        let r_ok = read_yuv_frame(&mut ref_file, r_pic.as_raw_mut())?;
        let d_ok = read_yuv_frame(&mut dist_file, d_pic.as_raw_mut())?;
        if !r_ok || !d_ok {
            break;
        }
        ctx.read_pictures(r_pic, d_pic, idx)?;
        n_frames = idx.saturating_add(1);
    }
    if n_frames == 0 {
        return Err("zero frames read from fixtures".into());
    }
    ctx.flush()?;
    let pooled = ctx.score_pooled(
        &model,
        vmafx::PoolingMethod::Mean,
        0,
        n_frames.saturating_sub(1),
    )?;
    Ok((pooled, n_frames))
}

fn assert_bitexact_scores(got: f64, want: f64) -> Result<(), Box<dyn std::error::Error>> {
    let got_bits = got.to_bits();
    let want_bits = want.to_bits();
    if got_bits != want_bits {
        return Err(format!(
            "bit mismatch: got 0x{got_bits:016x} ({got}), want 0x{want_bits:016x} ({want})"
        )
        .into());
    }
    Ok(())
}

#[test]
fn test_bitexact_comparison_rejects_one_ulp_difference() -> Result<(), Box<dyn std::error::Error>> {
    let ref_val = 82.81606015944988_f64;
    let bits = ref_val.to_bits();
    let perturbed = f64::from_bits(bits.wrapping_add(1));
    let res = assert_bitexact_scores(perturbed, ref_val);
    if res.is_ok() {
        return Err("comparison helper must reject 1 ULP difference".into());
    }
    Ok(())
}

#[test]
fn test_missing_input_fails_when_pair_is_required() -> Result<(), Box<dyn std::error::Error>> {
    let absent_paths = InputPaths {
        reference: PathBuf::from("nonexistent_ref.yuv"),
        distorted: PathBuf::from("nonexistent_dist.yuv"),
        model: PathBuf::from("nonexistent_model.json"),
        contract_bin: PathBuf::from("nonexistent_contract"),
    };
    let res = check_inputs(&absent_paths, true);
    if res.is_ok() {
        return Err("check_inputs must fail when require_pair is true".into());
    }
    Ok(())
}

#[test]
fn test_netflix_576x324_bitexact_with_c_reference() -> Result<(), Box<dyn std::error::Error>> {
    let paths = find_inputs();
    if !check_inputs(&paths, false)? {
        return Ok(());
    }
    let c_ref = run_c_contract(&paths)?;
    let (rust_score, rust_frames) = score_pair_rust(&paths)?;

    if rust_frames != c_ref.frames {
        return Err(format!(
            "frame count mismatch: rust {} != c {}",
            rust_frames, c_ref.frames
        )
        .into());
    }
    assert!(!c_ref.vmaf_hex.is_empty());
    assert_bitexact_scores(rust_score, c_ref.vmaf)?;
    Ok(())
}
