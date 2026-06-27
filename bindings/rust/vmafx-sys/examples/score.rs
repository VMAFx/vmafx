// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause
//
// examples/score.rs — smoke test: score the Netflix golden pair through the safe API.
//
// Expected mean VMAF score (CPU, vmaf_v0.6.1): 76.669 (places=3)
// Full value: 76.66890519623612
//
// Usage:
//   VMAFX_REPO=<repo-root> cargo run --example score
//
// Or with explicit paths:
//   VMAFX_YUV_REF=<path> VMAFX_YUV_DIST=<path> VMAFX_MODEL=<path> \
//       cargo run --example score

use std::env;
use std::fs::File;
use std::io::Read;
use std::path::PathBuf;

use vmafx_sys::safe::{VmafContext, VmafModel, alloc_yuv420p_8bit, unref_picture, version};

const WIDTH: u32 = 576;
const HEIGHT: u32 = 324;
const N_FRAMES: usize = 240;

fn repo_root() -> PathBuf {
    if let Ok(v) = env::var("VMAFX_REPO") {
        return PathBuf::from(v);
    }
    // Walk up from the manifest directory until we find a meson.build at root.
    let mut dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    loop {
        if dir.join("meson.build").exists() || dir.join("Cargo.toml").exists() {
            // Check this is actually the repo root (has a model/ dir).
            if dir.join("model").exists() {
                return dir;
            }
        }
        if !dir.pop() {
            break;
        }
    }
    // Fallback: assume we are running from the repo root.
    PathBuf::from(".")
}

// similar_names: cb_plane/cr_plane and src_cb/src_cr are standard YUV
// chroma-plane naming; renaming would obscure the domain meaning.
#[allow(clippy::similar_names)]
fn read_yuv_frame(file: &mut File, pic: &mut vmafx_sys::VmafPicture) -> std::io::Result<bool> {
    let w = WIDTH as usize;
    let h = HEIGHT as usize;

    // luma
    let luma_bytes = w * h;
    let mut luma = vec![0u8; luma_bytes];
    let n = file.read(&mut luma)?;
    if n == 0 {
        return Ok(false); // EOF
    }
    if n < luma_bytes {
        return Ok(false);
    }

    // chroma (4:2:0)
    let chroma_w = w / 2;
    let chroma_h = h / 2;
    let chroma_bytes = chroma_w * chroma_h;
    let mut cb = vec![0u8; chroma_bytes];
    let mut cr = vec![0u8; chroma_bytes];
    file.read_exact(&mut cb)?;
    file.read_exact(&mut cr)?;

    // Copy into the VmafPicture planes.
    // cast_sign_loss: libvmaf strides are always non-negative for a valid picture.
    #[allow(clippy::cast_sign_loss)]
    unsafe {
        let luma_plane = pic.data[0].cast::<u8>();
        let cb_plane = pic.data[1].cast::<u8>();
        let cr_plane = pic.data[2].cast::<u8>();

        let luma_stride = pic.stride[0] as usize;
        let cb_stride = pic.stride[1] as usize;
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

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let root = repo_root();

    let yuv_ref_path = env::var("VMAFX_YUV_REF").unwrap_or_else(|_| {
        root.join("python/test/resource/yuv/src01_hrc00_576x324.yuv")
            .to_string_lossy()
            .into_owned()
    });
    let yuv_dist_path = env::var("VMAFX_YUV_DIST").unwrap_or_else(|_| {
        root.join("python/test/resource/yuv/src01_hrc01_576x324.yuv")
            .to_string_lossy()
            .into_owned()
    });
    let model_path = env::var("VMAFX_MODEL").unwrap_or_else(|_| {
        root.join("model/vmaf_v0.6.1.json")
            .to_string_lossy()
            .into_owned()
    });

    println!("vmafx-sys version: {}", version());
    println!("Reference:  {yuv_ref_path}");
    println!("Distorted:  {yuv_dist_path}");
    println!("Model:      {model_path}");

    // Graceful skip when YUV fixtures are absent (CI without full test resources).
    // Pattern mirrors integration_test.rs: print SKIP and return Ok(()) so the
    // example exits 0 rather than failing with a misleading file-not-found error.
    if !std::path::Path::new(&yuv_ref_path).exists() {
        eprintln!("SKIP: reference YUV not found at {yuv_ref_path} — test fixtures absent");
        return Ok(());
    }
    if !std::path::Path::new(&yuv_dist_path).exists() {
        eprintln!("SKIP: distorted YUV not found at {yuv_dist_path} — test fixtures absent");
        return Ok(());
    }
    if !std::path::Path::new(&model_path).exists() {
        eprintln!("SKIP: model not found at {model_path} — model files absent");
        return Ok(());
    }

    let mut ctx = VmafContext::new()?;
    let mut model = VmafModel::from_path(&model_path)?;

    ctx.use_features_from_model(&mut model)?;

    let mut ref_file = File::open(&yuv_ref_path)
        .map_err(|e| format!("Cannot open reference YUV {yuv_ref_path}: {e}"))?;
    let mut dist_file = File::open(&yuv_dist_path)
        .map_err(|e| format!("Cannot open distorted YUV {yuv_dist_path}: {e}"))?;

    let mut n_frames = 0u32;
    loop {
        let mut ref_pic = alloc_yuv420p_8bit(WIDTH, HEIGHT)?;
        let mut dist_pic = alloc_yuv420p_8bit(WIDTH, HEIGHT)?;

        // Round-3 R3-14: `VmafPicture` from the `-sys` layer has no `Drop`, so a
        // bare `?` on a read error would leak the plane buffers of both
        // already-allocated pictures. Capture the results and unref both
        // pictures on ANY error before propagating it (mirrors the clean-EOF
        // path below, which already unrefs).
        let read_result = read_yuv_frame(&mut ref_file, &mut ref_pic)
            .and_then(|ref_ok| read_yuv_frame(&mut dist_file, &mut dist_pic).map(|d| (ref_ok, d)));
        let (ref_ok, dist_ok) = match read_result {
            Ok(oks) => oks,
            Err(e) => {
                unref_picture(&mut ref_pic)?;
                unref_picture(&mut dist_pic)?;
                return Err(e.into());
            }
        };

        if !ref_ok || !dist_ok {
            unref_picture(&mut ref_pic)?;
            unref_picture(&mut dist_pic)?;
            break;
        }

        ctx.read_pictures(&mut ref_pic, &mut dist_pic, n_frames)?;
        n_frames += 1;

        if n_frames as usize >= N_FRAMES {
            break;
        }
    }

    ctx.flush()?;

    if n_frames == 0 {
        eprintln!("ERROR: no frames read — check that the YUV paths are correct.");
        std::process::exit(1);
    }

    let score = ctx.score_pooled(&mut model, 0, n_frames - 1)?;
    println!("Frames processed: {n_frames}");
    println!("Mean VMAF score:  {score:.4}");

    // Golden assertion: 76.669 places=3 (full value: 76.66890519623612)
    // Python golden gate for this sequence uses places=2; we assert places=3 here.
    let expected = 76.669_f64;
    let tolerance = 5e-3; // places=3
    if (score - expected).abs() > tolerance {
        eprintln!(
            "ASSERTION FAILED: expected {expected:.4}, got {score:.4} (delta={:.6})",
            (score - expected).abs()
        );
        std::process::exit(2);
    }
    println!("Score assertion PASSED (expected {expected:.4})");
    Ok(())
}
