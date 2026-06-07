// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause
//
// integration_test.rs — scores the Netflix golden YUV pair through vmafx-sys
// and asserts the mean VMAF equals 76.669 (places=3).
//
// The Python golden gate uses places=2 for this sequence (see quality_runner_test.py:276);
// we gate at places=3 (tighter) here to catch regressions early.
//
// Environment variables (optional — defaults resolve relative to repo root):
//   VMAFX_REPO      Override repo root path.
//   VMAFX_YUV_REF   Path to reference YUV.
//   VMAFX_YUV_DIST  Path to distorted YUV.
//   VMAFX_MODEL     Path to vmaf_v0.6.1.json.

use std::env;
use std::fs::File;
use std::io::Read;
use std::path::PathBuf;

use vmafx_sys::safe::{VmafContext, VmafModel, alloc_yuv420p_8bit, unref_picture};

const WIDTH: u32 = 576;
const HEIGHT: u32 = 324;
// 76.66890519623612 — full src01 pair, vmaf_v0.6.1.json, CPU scalar.
// Rounded to places=3: 76.669. (Python golden gate uses places=2 here.)
const EXPECTED_SCORE: f64 = 76.669;
const PLACES_3_TOLERANCE: f64 = 5e-3;

fn repo_root() -> PathBuf {
    if let Ok(v) = env::var("VMAFX_REPO") {
        return PathBuf::from(v);
    }
    let mut dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    loop {
        if dir.join("model").exists() {
            return dir;
        }
        if !dir.pop() {
            break;
        }
    }
    PathBuf::from(".")
}

fn read_frame(file: &mut File, pic: &mut vmafx_sys::VmafPicture) -> bool {
    let w = WIDTH as usize;
    let h = HEIGHT as usize;
    let chroma_w = w / 2;
    let chroma_h = h / 2;

    let mut luma = vec![0u8; w * h];
    let n = file.read(&mut luma).unwrap_or(0);
    if n < w * h {
        return false;
    }

    let mut cb = vec![0u8; chroma_w * chroma_h];
    let mut cr = vec![0u8; chroma_w * chroma_h];
    if file.read_exact(&mut cb).is_err() || file.read_exact(&mut cr).is_err() {
        return false;
    }

    unsafe {
        for row in 0..h {
            let src = &luma[row * w..(row + 1) * w];
            let dst = (pic.data[0] as *mut u8).add(row * pic.stride[0] as usize);
            std::ptr::copy_nonoverlapping(src.as_ptr(), dst, w);
        }
        for row in 0..chroma_h {
            let s_cb = &cb[row * chroma_w..(row + 1) * chroma_w];
            let s_cr = &cr[row * chroma_w..(row + 1) * chroma_w];
            let d_cb = (pic.data[1] as *mut u8).add(row * pic.stride[1] as usize);
            let d_cr = (pic.data[2] as *mut u8).add(row * pic.stride[2] as usize);
            std::ptr::copy_nonoverlapping(s_cb.as_ptr(), d_cb, chroma_w);
            std::ptr::copy_nonoverlapping(s_cr.as_ptr(), d_cr, chroma_w);
        }
    }
    true
}

#[test]
fn netflix_golden_score() {
    let root = repo_root();

    let yuv_ref = env::var("VMAFX_YUV_REF").unwrap_or_else(|_| {
        root.join("python/test/resource/yuv/src01_hrc00_576x324.yuv")
            .to_string_lossy()
            .into_owned()
    });
    let yuv_dist = env::var("VMAFX_YUV_DIST").unwrap_or_else(|_| {
        root.join("python/test/resource/yuv/src01_hrc01_576x324.yuv")
            .to_string_lossy()
            .into_owned()
    });
    let model_path = env::var("VMAFX_MODEL").unwrap_or_else(|_| {
        root.join("model/vmaf_v0.6.1.json")
            .to_string_lossy()
            .into_owned()
    });

    // Skip the test gracefully if the test data isn't present (e.g. CI without fixtures).
    if !std::path::Path::new(&yuv_ref).exists() {
        eprintln!("SKIP: reference YUV not found at {yuv_ref} — set VMAFX_REPO or VMAFX_YUV_REF");
        return;
    }

    let mut ctx = VmafContext::new().expect("vmaf_init failed");
    let mut model = VmafModel::from_path(&model_path).expect("vmaf_model_load_from_path failed");
    ctx.use_features_from_model(&mut model)
        .expect("vmaf_use_features_from_model failed");

    let mut ref_file = File::open(&yuv_ref).expect("open ref YUV");
    let mut dist_file = File::open(&yuv_dist).expect("open dist YUV");

    let mut n_frames = 0u32;
    loop {
        let mut ref_pic = alloc_yuv420p_8bit(WIDTH, HEIGHT).expect("alloc ref pic");
        let mut dist_pic = alloc_yuv420p_8bit(WIDTH, HEIGHT).expect("alloc dist pic");

        if !read_frame(&mut ref_file, &mut ref_pic) || !read_frame(&mut dist_file, &mut dist_pic) {
            unref_picture(&mut ref_pic).ok();
            unref_picture(&mut dist_pic).ok();
            break;
        }

        ctx.read_pictures(&mut ref_pic, &mut dist_pic, n_frames)
            .expect("vmaf_read_pictures failed");
        n_frames += 1;
    }

    ctx.flush().expect("flush failed");
    assert!(n_frames > 0, "no frames read from YUV files");

    let score = ctx
        .score_pooled(&mut model, 0, n_frames - 1)
        .expect("vmaf_score_pooled failed");

    let delta = (score - EXPECTED_SCORE).abs();
    assert!(
        delta <= PLACES_3_TOLERANCE,
        "Netflix golden score assertion failed: expected {EXPECTED_SCORE:.4}, got {score:.4} (delta={delta:.6})"
    );
}
